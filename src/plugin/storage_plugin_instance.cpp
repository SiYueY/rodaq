#include "plugin/storage_plugin_instance.hpp"

#include "plugin/plugin_status.hpp"
#include "plugin/plugin_validation.hpp"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace rodaq::plugin {

Result<StoragePluginInstance> StoragePluginInstance::create(PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Storage) {
        return invalid_argument_status(
            "Storage instance requires a valid Storage PluginLibraryLease");
    }
    if (library_lease.quarantined()) {
        return rodaq::Status(
            StatusCode::Unavailable, "plugin", 0,
            "Cannot create a Storage instance from a quarantined Plugin");
    }

    const rodaq_storage_plugin* protocol = library_lease.storage_protocol();
    if (protocol == nullptr || protocol->create == nullptr || protocol->destroy == nullptr) {
        return invalid_argument_status("Storage family protocol is missing create/destroy");
    }

    void* raw_instance = nullptr;
    rodaq_status status_error{};
    const rodaq_status_code status = protocol->create(&raw_instance, &status_error);
    if (status != RODAQ_STATUS_OK) {
        if (raw_instance != nullptr) {
            library_lease.quarantine();
        }
        Status converted = status_from_abi(status, &status_error, "plugin");
        return converted;
    }
    if (raw_instance == nullptr) {
        return invalid_argument_status("Storage create() returned OK with a null instance");
    }

    try {
        auto state = PluginInstanceStateAccess::create(
            library_lease, raw_instance, protocol->destroy, protocol->bind, protocol->configure,
            protocol->query_interface, nullptr, nullptr, protocol->open, protocol->submit,
            protocol->flush, protocol->close);
        return StoragePluginInstance(std::move(state));
    } catch (const std::bad_alloc&) {
        protocol->destroy(raw_instance);
        return rodaq::Status(
            StatusCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while tracking Storage instance lifetime");
    } catch (const std::exception& error) {
        protocol->destroy(raw_instance);
        return internal_status(
            std::string("Failed to track Storage instance lifetime: ") + error.what());
    }
}

StoragePluginInstance::StoragePluginInstance(std::shared_ptr<PluginInstanceState> state) noexcept
: state_(std::move(state)) {}

Result<void> StoragePluginInstance::bind(const rodaq_service_registry& services) {
    Result<void> bind_result = PluginInstanceStateAccess::bind(state_, services);
    if (!bind_result.ok()) {
        return bind_result;
    }
    Result<rodaq_interface> control_result =
        PluginInstanceStateAccess::query_interface(state_, RODAQ_STORAGE_CONTROL_INTERFACE_ID);
    if (!control_result.ok()) {
        return std::move(control_result).status();
    }
    Result<rodaq_interface> validated = validate_storage_control_interface(control_result.value());
    if (!validated.ok()) {
        return std::move(validated).status();
    }
    PluginInstanceStateAccess::set_required_control_interface(state_, validated.value());
    return {};
}

Result<void> StoragePluginInstance::configure(rodaq_config_view config) {
    return PluginInstanceStateAccess::configure(state_, config);
}

Result<void> StoragePluginInstance::open(const rodaq_storage_open_request& request) {
    return PluginInstanceStateAccess::open(state_, request);
}

Result<rodaq_storage_submit_result> StoragePluginInstance::submit(
    const rodaq_storage_submission& submission) {
    return PluginInstanceStateAccess::submit(state_, submission);
}

Result<void> StoragePluginInstance::flush() { return PluginInstanceStateAccess::flush(state_); }

Result<void> StoragePluginInstance::close(const rodaq_storage_close_request& request) {
    return PluginInstanceStateAccess::close(state_, request);
}

void StoragePluginInstance::destroy() noexcept { PluginInstanceStateAccess::destroy(state_); }

Result<rodaq_interface> StoragePluginInstance::query_interface(std::string_view interface_id) {
    return PluginInstanceStateAccess::query_interface(state_, interface_id);
}

Result<rodaq_interface> StoragePluginInstance::required_control_interface() const {
    return PluginInstanceStateAccess::required_control_interface(state_);
}

PluginInstanceLease StoragePluginInstance::lease() const noexcept {
    return PluginInstanceLease(state_);
}

bool StoragePluginInstance::valid() const noexcept { return state_ != nullptr; }

}  // namespace rodaq::plugin
