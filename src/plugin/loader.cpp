#include "loader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/sysmacros.h>
#include <utility>

#include <rodaq/plugin/abi.hpp>

namespace rodaq::plugin {
namespace {

inline constexpr std::size_t kMaxResidentLibraries = 256;

struct ResidentSlot {
    bool reserved{false};
    std::shared_ptr<LibraryState> library;
};

struct ResidentLibraries {
    std::mutex mutex;
    std::array<ResidentSlot, kMaxResidentLibraries> slots;
};

ResidentLibraries& residents() {
    // Explicit process-resident owner: static destruction must not race native
    // work during controlled restart, and successful mappings are never unloaded.
    static auto* const libraries = new ResidentLibraries;
    return *libraries;
}

// Distinguish an exception escaping foreign code from a Framework allocation
// failure. Even foreign std::bad_alloc is an ABI violation, not a safe retry.
struct NativeAbiViolation {};

template <typename Function>
auto bootstrap_call(Function&& function) -> decltype(function()) {
    try {
        return function();
    } catch (...) {
        throw NativeAbiViolation{};
    }
}

bool matches_metadata(const PluginInfo& left, const PluginInfo& right) noexcept {
    return left.plugin_id == right.plugin_id && left.plugin_name == right.plugin_name &&
           left.plugin_version == right.plugin_version;
}

bool valid_name(rodaq_string_t name) noexcept {
    return name.size > 0 && name.size <= 255 && abi::valid_string(name) &&
           std::memchr(name.data, 0, name.size) == nullptr;
}

Result<void> validate_descriptor(
    const rodaq_plugin_descriptor_t* descriptor, const PluginInfo& expected) {
    if (descriptor == nullptr ||
        reinterpret_cast<std::uintptr_t>(descriptor) % alignof(rodaq_plugin_descriptor_t) != 0 ||
        descriptor->header.struct_size < offsetof(rodaq_plugin_descriptor_t, plugin_version) +
                                             sizeof(descriptor->plugin_version) ||
        !valid_name(descriptor->plugin_name) || descriptor->plugin_id.data == nullptr ||
        descriptor->plugin_id.size > 255 ||
        !abi::valid_identifier({descriptor->plugin_id.data, descriptor->plugin_id.size}) ||
        descriptor->plugin_version.data == nullptr || descriptor->plugin_version.size == 0 ||
        descriptor->plugin_version.size > 128) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin descriptor");
    }
    const std::string_view version(
        descriptor->plugin_version.data, descriptor->plugin_version.size);
    if (!parse_version(version).ok() ||
        expected.plugin_id !=
            std::string_view(descriptor->plugin_id.data, descriptor->plugin_id.size) ||
        expected.plugin_name !=
            std::string_view(descriptor->plugin_name.data, descriptor->plugin_name.size) ||
        expected.plugin_version != version) {
        return Status(StatusCode::InvalidArgument, "Plugin descriptor/Manifest mismatch");
    }
    return {};
}

bool mapping_matches(void* address, const FileIdentity& identity) {
    // dladdr alone cannot detect a pathname replaced while an older ELF mapping
    // remains resident. Verify the inode/device of the actual executable VMA.
    Dl_info information{};
    if (dladdr(address, &information) == 0 || information.dli_fname == nullptr) {
        return false;
    }
    const auto named = file_identity(information.dli_fname);
    if (!named.ok() || !same_file(named.value(), identity)) {
        return false;
    }
    std::ifstream maps("/proc/self/maps");
    std::array<char, 8192> line{};
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    std::size_t lines = 0;
    while (maps.getline(line.data(), line.size()) && ++lines <= 65536) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        unsigned long long offset = 0;
        unsigned long long inode = 0;
        unsigned int device_major = 0;
        unsigned int device_minor = 0;
        std::array<char, 5> permissions{};
        const auto fields = std::sscanf(
            line.data(), "%llx-%llx %4s %llx %x:%x %llu", &start, &end, permissions.data(), &offset,
            &device_major, &device_minor, &inode);
        if (fields == 7 && target >= start && target < end) {
            return permissions[2] == 'x' && inode == identity.inode &&
                   device_major == major(identity.device) && device_minor == minor(identity.device);
        }
    }
    return false;
}

}  // namespace

PreparedLibrary::PreparedLibrary(
    std::shared_ptr<LibraryState> library, std::optional<std::size_t> slot) noexcept
: library_(std::move(library)), slot_(slot) {}

PreparedLibrary::PreparedLibrary(PreparedLibrary&& other) noexcept
: library_(std::move(other.library_)),
  handle_(std::exchange(other.handle_, nullptr)),
  slot_(std::exchange(other.slot_, std::nullopt)) {}

PreparedLibrary::~PreparedLibrary() {
    if (handle_ != nullptr) {
        dlclose(handle_);
    }
    if (slot_) {
        auto& cache = residents();
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.slots[*slot_].reserved = false;
    }
}

Result<void> PreparedLibrary::commit() {
    if (!slot_) {
        return {};
    }
    auto& cache = residents();
    std::lock_guard<std::mutex> lock(cache.mutex);
    auto& slot = cache.slots[*slot_];
    if (!slot.reserved || slot.library || handle_ == nullptr) {
        return Status(StatusCode::Internal, "Invalid resident load reservation");
    }
    library_->native_handle = handle_;
    slot.library = library_;
    slot.reserved = false;
    slot_.reset();
    handle_ = nullptr;
    return {};
}

Result<PreparedLibrary> prepare_library(const CatalogEntry& entry) {
    auto verified = verify_artifact(entry);
    if (!verified.ok()) {
        return verified.status();
    }
    auto& cache = residents();
    std::optional<std::size_t> reservation;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (std::size_t i = 0; i < cache.slots.size(); ++i) {
            const auto& slot = cache.slots[i];
            if (slot.library && same_file(slot.library->identity, verified.value()) &&
                slot.library->binary.sha256 == entry.binary.sha256) {
                if (!matches_metadata(slot.library->info, entry.info)) {
                    return Status(StatusCode::Conflict, "Resident Artifact metadata conflict");
                }
                return PreparedLibrary(slot.library, {});
            }
            if (slot.library && slot.library->path == entry.artifact) {
                return Status(StatusCode::Conflict, "Resident Plugin path was replaced");
            }
            if (!slot.reserved && !slot.library && !reservation) {
                reservation = i;
            }
        }
        if (!reservation) {
            return Status(StatusCode::ResourceExhausted, "Process resident Plugin limit");
        }
        cache.slots[*reservation].reserved = true;
    }
    // Establish rollback ownership before allocations or any dlopen initializer.
    PreparedLibrary prepared({}, reservation);
    prepared.library_ = std::make_shared<LibraryState>();
    auto& library = *prepared.library_;
    library.info = entry.info;
    library.binary = entry.binary;
    library.path = entry.artifact;
    library.identity = verified.value();
    prepared.handle_ =
        bootstrap_call([&] { return dlopen(entry.artifact.c_str(), RTLD_NOW | RTLD_LOCAL); });
    if (prepared.handle_ == nullptr) {
        const auto* message = dlerror();
        return Status(
            StatusCode::Unavailable,
            message == nullptr ? "dlopen failed" : std::string(message, strnlen(message, 256)));
    }
    void* version_address = dlsym(prepared.handle_, "rodaq_plugin_abi_version");
    if (version_address == nullptr || !mapping_matches(version_address, library.identity)) {
        return Status(StatusCode::InvalidArgument, "Plugin ABI symbol provenance failed");
    }
    uint32_t (*version)(void) = nullptr;
    static_assert(sizeof(version) == sizeof(version_address), "POSIX function pointer size");
    std::memcpy(&version, &version_address, sizeof(version));
    const auto required = bootstrap_call(version);
    if (RODAQ_PLUGIN_ABI_MAJOR(required) != entry.abi_major ||
        RODAQ_PLUGIN_ABI_MINOR(required) != entry.abi_minor ||
        RODAQ_PLUGIN_ABI_MAJOR(required) != RODAQ_PLUGIN_ABI_MAJOR(RODAQ_PLUGIN_ABI_VERSION) ||
        RODAQ_PLUGIN_ABI_MINOR(required) > RODAQ_PLUGIN_ABI_MINOR(RODAQ_PLUGIN_ABI_VERSION)) {
        return Status(StatusCode::Unavailable, "Plugin ABI version mismatch");
    }
    void* entry_address = dlsym(prepared.handle_, "rodaq_plugin_get_entry");
    if (entry_address == nullptr || !mapping_matches(entry_address, library.identity)) {
        return Status(StatusCode::InvalidArgument, "Plugin Entry symbol provenance failed");
    }
    const rodaq_plugin_entry_t* (*get_entry)(void) = nullptr;
    static_assert(sizeof(get_entry) == sizeof(entry_address), "POSIX function pointer size");
    std::memcpy(&get_entry, &entry_address, sizeof(get_entry));
    const auto* native = bootstrap_call(get_entry);
    constexpr auto kRequired =
        offsetof(rodaq_plugin_entry_t, create) + sizeof(rodaq_plugin_entry_t::create);
    if (native == nullptr ||
        reinterpret_cast<std::uintptr_t>(native) % alignof(rodaq_plugin_entry_t) != 0 ||
        native->header.struct_size < kRequired) {
        return Status(StatusCode::InvalidArgument, "Truncated Plugin Entry");
    }
    std::memcpy(&library.entry, native, kRequired);
    if (library.entry.descriptor == nullptr || library.entry.create == nullptr) {
        return Status(StatusCode::InvalidArgument, "Missing Plugin Entry slot");
    }
    const auto descriptor =
        validate_descriptor(bootstrap_call(library.entry.descriptor), library.info);
    if (!descriptor.ok()) {
        return descriptor.status();
    }
    const auto after = file_identity(entry.artifact);
    if (!after.ok() || !same_file(library.identity, after.value())) {
        return Status(StatusCode::DataLoss, "Plugin artifact changed during load");
    }
    return prepared;
}

}  // namespace rodaq::plugin
