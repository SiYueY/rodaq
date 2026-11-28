#include "loader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
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
    const rodaq_plugin_descriptor_t* descriptor, const CatalogEntry& expected) {
    constexpr auto kRequiredDescriptorPrefix = offsetof(rodaq_plugin_descriptor_t, abi_revision) +
                                               sizeof(rodaq_plugin_descriptor_t::abi_revision);
    if (descriptor == nullptr ||
        reinterpret_cast<std::uintptr_t>(descriptor) % alignof(rodaq_plugin_descriptor_t) != 0 ||
        descriptor->header.struct_size < kRequiredDescriptorPrefix ||
        !valid_name(descriptor->plugin_name) || descriptor->plugin_id.data == nullptr ||
        descriptor->plugin_id.size > 255 ||
        !abi::valid_identifier({descriptor->plugin_id.data, descriptor->plugin_id.size}) ||
        descriptor->plugin_version.data == nullptr || descriptor->plugin_version.size == 0 ||
        descriptor->plugin_version.size > 128) {
        return Status(StatusCode::InvalidArgument, "Invalid Plugin descriptor");
    }
    if (descriptor->abi_revision != RODAQ_PLUGIN_ABI_REVISION ||
        descriptor->abi_revision != expected.abi_revision) {
        return Status(StatusCode::Unavailable, "Incompatible Plugin ABI revision");
    }
    const std::string_view version(
        descriptor->plugin_version.data, descriptor->plugin_version.size);
    if (!parse_semver(version).ok() ||
        expected.info.plugin_id !=
            std::string_view(descriptor->plugin_id.data, descriptor->plugin_id.size) ||
        expected.info.plugin_name !=
            std::string_view(descriptor->plugin_name.data, descriptor->plugin_name.size) ||
        expected.info.plugin_version != version) {
        return Status(StatusCode::InvalidArgument, "Plugin descriptor/Manifest mismatch");
    }
    return {};
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
    auto& cache = residents();
    std::optional<std::size_t> reservation;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        for (std::size_t i = 0; i < cache.slots.size(); ++i) {
            const auto& slot = cache.slots[i];
            if (slot.library && slot.library->path == entry.library_path) {
                if (!matches_metadata(slot.library->info, entry.info)) {
                    return Status(StatusCode::Conflict, "Resident Plugin metadata conflict");
                }
                return PreparedLibrary(slot.library, {});
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
    library.path = entry.library_path;
    prepared.handle_ =
        bootstrap_call([&] { return dlopen(entry.library_path.c_str(), RTLD_NOW | RTLD_LOCAL); });
    if (prepared.handle_ == nullptr) {
        const auto* message = dlerror();
        return Status(
            StatusCode::Unavailable,
            message == nullptr ? "dlopen failed" : std::string(message, strnlen(message, 256)));
    }
    void* version_address = dlsym(prepared.handle_, "rodaq_plugin_abi_version");
    if (version_address == nullptr) {
        return Status(StatusCode::InvalidArgument, "Missing rodaq_plugin_abi_version symbol");
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
    if (entry_address == nullptr) {
        return Status(StatusCode::InvalidArgument, "Missing rodaq_plugin_get_entry symbol");
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
    const auto descriptor = validate_descriptor(bootstrap_call(library.entry.descriptor), entry);
    if (!descriptor.ok()) {
        return descriptor.status();
    }
    return prepared;
}

}  // namespace rodaq::plugin
