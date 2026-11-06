#include "plugin/acquisition_plugin_instance.hpp"

#include "plugin/plugin_status.hpp"
#include "plugin/plugin_validation.hpp"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace rodaq::plugin {

Result<AcquisitionPluginInstance> AcquisitionPluginInstance::create(
    PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Acquisition) {
        return invalid_argument_status(
            "Acquisition instance requires a valid Acquisition PluginLibraryLease");
    }
    if (library_lease.quarantined()) {
        return rodaq::Status(
            StatusCode::Unavailable, "plugin", 0,
            "Cannot create an Acquisition instance from a quarantined Plugin");
    }

    const rodaq_acquisition_plugin* protocol = library_lease.acquisition_protocol();
    if (protocol == nullptr || protocol->create == nullptr || protocol->destroy == nullptr) {
        return invalid_argument_status("Acquisition family protocol is missing create/destroy");
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
        return invalid_argument_status("Acquisition create() returned OK with a null instance");
    }

    try {
        auto state = PluginInstanceStateAccess::create(
            library_lease, raw_instance, protocol->destroy, protocol->bind, protocol->configure,
            protocol->query_interface, protocol->start, protocol->stop, nullptr, nullptr, nullptr,
            nullptr);
        return AcquisitionPluginInstance(std::move(state));
    } catch (const std::bad_alloc&) {
        protocol->destroy(raw_instance);
        return rodaq::Status(
            StatusCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while tracking Acquisition instance lifetime");
    } catch (const std::exception& error) {
        protocol->destroy(raw_instance);
        return internal_status(
            std::string("Failed to track Acquisition instance lifetime: ") + error.what());
    }
}

AcquisitionPluginInstance::AcquisitionPluginInstance(
    std::shared_ptr<PluginInstanceState> state) noexcept
: state_(std::move(state)) {}

Result<void> AcquisitionPluginInstance::bind(const rodaq_service_registry& services) {
    Result<void> bind_result = PluginInstanceStateAccess::bind(state_, services);
    if (!bind_result.ok()) {
        return bind_result;
    }
    Result<rodaq_interface> control_result =
        PluginInstanceStateAccess::query_interface(state_, RODAQ_ACQUISITION_CONTROL_INTERFACE_ID);
    if (!control_result.ok()) {
        return std::move(control_result).status();
    }
    Result<rodaq_interface> validated =
        validate_acquisition_control_interface(control_result.value());
    if (!validated.ok()) {
        return std::move(validated).status();
    }
    PluginInstanceStateAccess::set_required_control_interface(state_, validated.value());
    return {};
}

Result<void> AcquisitionPluginInstance::configure(rodaq_config_view config) {
    return PluginInstanceStateAccess::configure(state_, config);
}

Result<void> AcquisitionPluginInstance::start() { return PluginInstanceStateAccess::start(state_); }

Result<void> AcquisitionPluginInstance::stop() { return PluginInstanceStateAccess::stop(state_); }

void AcquisitionPluginInstance::destroy() noexcept { PluginInstanceStateAccess::destroy(state_); }

Result<rodaq_interface> AcquisitionPluginInstance::query_interface(std::string_view interface_id) {
    return PluginInstanceStateAccess::query_interface(state_, interface_id);
}

Result<rodaq_interface> AcquisitionPluginInstance::required_control_interface() const {
    return PluginInstanceStateAccess::required_control_interface(state_);
}

PluginInstanceLease AcquisitionPluginInstance::lease() const noexcept {
    return PluginInstanceLease(state_);
}

bool AcquisitionPluginInstance::valid() const noexcept { return state_ != nullptr; }

}  // namespace rodaq::plugin
