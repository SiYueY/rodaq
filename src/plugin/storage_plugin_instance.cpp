#include "plugin/storage_plugin_instance.hpp"

#include "plugin/plugin_error.hpp"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace rodaq::plugin {

Result<StoragePluginInstance> StoragePluginInstance::create(PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Storage) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage instance requires a valid Storage PluginLibraryLease"));
    }
    if (library_lease.quarantined()) {
        return nonstd::make_unexpected(Error(
            ErrorCode::Unavailable, "plugin", 0,
            "Cannot create a Storage instance from a quarantined Plugin"));
    }

    const rodaq_storage_plugin* protocol = library_lease.storage_protocol();
    if (protocol == nullptr || protocol->create == nullptr || protocol->destroy == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage family protocol is missing create/destroy"));
    }

    void* raw_instance = nullptr;
    rodaq_error status_error{};
    const rodaq_status status = protocol->create(&raw_instance, &status_error);
    if (status != RODAQ_STATUS_OK) {
        if (raw_instance != nullptr) {
            library_lease.quarantine();
        }
        Result<void> converted = result_from_status(status, &status_error, "plugin");
        return nonstd::make_unexpected(std::move(converted.error()));
    }
    if (raw_instance == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Storage create() returned OK with a null instance"));
    }

    try {
        auto state =
            PluginInstanceStateAccess::create(library_lease, raw_instance, protocol->destroy);
        return StoragePluginInstance(std::move(state));
    } catch (const std::bad_alloc&) {
        protocol->destroy(raw_instance);
        return nonstd::make_unexpected(Error(
            ErrorCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while tracking Storage instance lifetime"));
    } catch (const std::exception& error) {
        protocol->destroy(raw_instance);
        return nonstd::make_unexpected(internal_error(
            std::string("Failed to track Storage instance lifetime: ") + error.what()));
    }
}

StoragePluginInstance::StoragePluginInstance(std::shared_ptr<PluginInstanceState> state) noexcept
: state_(std::move(state)) {}

PluginInstanceLease StoragePluginInstance::lease() const noexcept {
    return PluginInstanceLease(state_);
}

bool StoragePluginInstance::valid() const noexcept { return state_ != nullptr; }

}  // namespace rodaq::plugin
