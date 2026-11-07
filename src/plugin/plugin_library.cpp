#include "plugin/plugin_library.hpp"

#include "plugin/plugin_status.hpp"
#include "plugin/plugin_validation.hpp"
#include "plugin/shared_library.hpp"

#include <atomic>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace rodaq::plugin {
namespace {

using AcquisitionEntrypoint = const rodaq_acquisition_plugin* (*)() noexcept;
using StorageEntrypoint = const rodaq_storage_plugin* (*)() noexcept;

template <typename FunctionPointer>
FunctionPointer function_from_symbol(void* symbol) noexcept {
    static_assert(sizeof(FunctionPointer) == sizeof(symbol));
    FunctionPointer function = nullptr;
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

Result<void*> find_optional_symbol(const SharedLibrary& library, std::string_view name) {
    Result<void*> symbol = library.find_symbol(name);
    if (symbol.ok() || symbol.status().code() != StatusCode::NotFound) {
        return symbol;
    }
    return static_cast<void*>(nullptr);
}

Result<void> require_owned_symbol(
    const SharedLibrary& library, void* symbol, std::string_view name) {
    if (symbol == nullptr) {
        return rodaq::Status(
            StatusCode::NotFound, "dlsym", 0,
            "Required Plugin symbol was not found: " + std::string(name));
    }
    if (!library.defines_symbol(symbol)) {
        return invalid_argument_status(
            "Plugin symbol is not defined by the candidate DSO: " + std::string(name));
    }
    return {};
}

}  // namespace

class PluginLibraryState {
public:
    PluginLibraryState(
        SharedLibrary owned_library, PluginFamily plugin_family,
        const rodaq_plugin_descriptor* descriptor,
        const rodaq_acquisition_plugin* owned_acquisition_protocol,
        const rodaq_storage_plugin* owned_storage_protocol, PluginMetadata owned_metadata)
    : library_(std::move(owned_library)),
      family_(plugin_family),
      raw_descriptor_(descriptor),
      acquisition_protocol_(owned_acquisition_protocol),
      storage_protocol_(owned_storage_protocol),
      metadata_(std::move(owned_metadata)) {}

    ~PluginLibraryState() {
        if (quarantined_.load(std::memory_order_acquire)) {
            library_.retain_until_process_exit();
        }
    }

    SharedLibrary library_;
    PluginFamily family_;
    const rodaq_plugin_descriptor* raw_descriptor_;
    const rodaq_acquisition_plugin* acquisition_protocol_;
    const rodaq_storage_plugin* storage_protocol_;
    PluginMetadata metadata_;
    std::atomic<bool> quarantined_{false};
};

Result<PluginLibrary> PluginLibrary::open(const std::filesystem::path& path) {
    try {
        Result<SharedLibrary> library_result = SharedLibrary::open(path);
        if (!library_result.ok()) {
            return std::move(library_result).status();
        }
        SharedLibrary library = std::move(library_result.value());

        Result<void*> abi_symbol = library.find_symbol("rodaq_plugin_abi_version");
        if (!abi_symbol.ok()) {
            return std::move(abi_symbol).status();
        }
        Result<void> marker_provenance =
            require_owned_symbol(library, abi_symbol.value(), "rodaq_plugin_abi_version");
        if (!marker_provenance.ok()) {
            return std::move(marker_provenance).status();
        }

        const auto* abi_version = static_cast<const uint32_t*>(abi_symbol.value());
        if (*abi_version != RODAQ_PLUGIN_ABI_VERSION) {
            return invalid_argument_status("Plugin DSO reports an unsupported Boot ABI version");
        }

        Result<void*> acquisition_symbol =
            find_optional_symbol(library, "rodaq_get_acquisition_plugin");
        if (!acquisition_symbol.ok()) {
            return std::move(acquisition_symbol).status();
        }
        Result<void*> storage_symbol = find_optional_symbol(library, "rodaq_get_storage_plugin");
        if (!storage_symbol.ok()) {
            return std::move(storage_symbol).status();
        }

        const bool has_acquisition = acquisition_symbol.value() != nullptr;
        const bool has_storage = storage_symbol.value() != nullptr;
        if (!has_acquisition && !has_storage) {
            return rodaq::Status(
                StatusCode::NotFound, "dlsym", 0,
                "Plugin DSO exports neither supported family entrypoint");
        }
        if (has_acquisition && has_storage) {
            return invalid_argument_status(
                "Plugin DSO exports both Acquisition and Storage family entrypoints");
        }

        const rodaq_plugin_descriptor* raw_descriptor = nullptr;
        const rodaq_acquisition_plugin* acquisition_protocol = nullptr;
        const rodaq_storage_plugin* storage_protocol = nullptr;
        PluginFamily family = PluginFamily::Acquisition;

        if (has_acquisition) {
            Result<void> provenance = require_owned_symbol(
                library, acquisition_symbol.value(), "rodaq_get_acquisition_plugin");
            if (!provenance.ok()) {
                return std::move(provenance).status();
            }

            const AcquisitionEntrypoint entrypoint =
                function_from_symbol<AcquisitionEntrypoint>(acquisition_symbol.value());
            acquisition_protocol = entrypoint();
            if (acquisition_protocol == nullptr) {
                return invalid_argument_status("Acquisition family entrypoint returned null");
            }
            Result<void> protocol_valid = validate_acquisition_protocol(*acquisition_protocol);
            if (!protocol_valid.ok()) {
                return std::move(protocol_valid).status();
            }
            raw_descriptor = acquisition_protocol->descriptor;
        } else {
            Result<void> provenance =
                require_owned_symbol(library, storage_symbol.value(), "rodaq_get_storage_plugin");
            if (!provenance.ok()) {
                return std::move(provenance).status();
            }

            const StorageEntrypoint entrypoint =
                function_from_symbol<StorageEntrypoint>(storage_symbol.value());
            storage_protocol = entrypoint();
            if (storage_protocol == nullptr) {
                return invalid_argument_status("Storage family entrypoint returned null");
            }
            Result<void> protocol_valid = validate_storage_protocol(*storage_protocol);
            if (!protocol_valid.ok()) {
                return std::move(protocol_valid).status();
            }
            raw_descriptor = storage_protocol->descriptor;
            family = PluginFamily::Storage;
        }

        Result<PluginMetadata> metadata_result = validate_plugin_descriptor(*raw_descriptor);
        if (!metadata_result.ok()) {
            return std::move(metadata_result).status();
        }

        auto state = std::make_shared<PluginLibraryState>(
            std::move(library), family, raw_descriptor, acquisition_protocol, storage_protocol,
            std::move(metadata_result.value()));
        return PluginLibrary(std::move(state));
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while loading Plugin library");
    } catch (const std::exception& error) {
        return internal_status(
            std::string("Unexpected Plugin library load failure: ") + error.what());
    }
}

PluginLibrary::PluginLibrary(std::shared_ptr<PluginLibraryState> state) noexcept
: state_(std::move(state)) {}

PluginLibraryLease PluginLibrary::lease() const noexcept { return PluginLibraryLease(state_); }

bool PluginLibrary::valid() const noexcept { return state_ != nullptr; }

PluginFamily PluginLibrary::family() const noexcept { return state_->family_; }

const PluginMetadata& PluginLibrary::metadata() const noexcept { return state_->metadata_; }

const std::filesystem::path& PluginLibrary::canonical_path() const noexcept {
    return state_->library_.canonical_path();
}

PluginLibraryLease::PluginLibraryLease(std::shared_ptr<PluginLibraryState> state) noexcept
: state_(std::move(state)) {}

bool PluginLibraryLease::valid() const noexcept { return state_ != nullptr; }

PluginFamily PluginLibraryLease::family() const noexcept { return state_->family_; }

const PluginMetadata& PluginLibraryLease::metadata() const noexcept { return state_->metadata_; }

const std::filesystem::path& PluginLibraryLease::canonical_path() const noexcept {
    return state_->library_.canonical_path();
}

const rodaq_plugin_descriptor* PluginLibraryLease::raw_descriptor() const noexcept {
    return state_->raw_descriptor_;
}

const rodaq_acquisition_plugin* PluginLibraryLease::acquisition_protocol() const noexcept {
    return state_->acquisition_protocol_;
}

const rodaq_storage_plugin* PluginLibraryLease::storage_protocol() const noexcept {
    return state_->storage_protocol_;
}

void PluginLibraryLease::quarantine() noexcept {
    if (state_ != nullptr) {
        state_->quarantined_.store(true, std::memory_order_release);
    }
}

bool PluginLibraryLease::quarantined() const noexcept {
    return state_ != nullptr && state_->quarantined_.load(std::memory_order_acquire);
}

}  // namespace rodaq::plugin
