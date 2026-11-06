#include "activation/activation_error.hpp"
#include "activation/activation_state.hpp"
#include "plugin/service_registry.hpp"

namespace rodaq {

Result<void> ActivationState::register_acquisition_services(
    plugin::ServiceRegistry& registry, std::size_t plugin_index) {
    if (plugin_index >= acquisition_contexts_.size()) {
        return activation_status(
            StatusCode::InvalidArgument, "Acquisition service index is invalid");
    }
    auto result = registry.register_entry(
        RODAQ_RUNTIME_COMMON_SERVICE_ID, rodaq_interface{this, &kCommonVtable});
    if (!result.ok()) {
        return result;
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_CANONICAL_SERVICE_ID, rodaq_interface{this, &kCanonicalVtable});
    if (!result.ok()) {
        return result;
    }
    result =
        registry.register_entry(RODAQ_RUNTIME_PROJECTION_SERVICE_ID, projections_.abi_interface());
    if (!result.ok()) {
        return result;
    }
    return registry.register_entry(
        RODAQ_RUNTIME_ACQUISITION_SERVICE_ID, acquisition_service_interface(plugin_index));
}

Result<void> ActivationState::register_storage_services(plugin::ServiceRegistry& registry) {
    auto result = registry.register_entry(
        RODAQ_RUNTIME_COMMON_SERVICE_ID, rodaq_interface{this, &kCommonVtable});
    if (!result.ok()) {
        return result;
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_CANONICAL_SERVICE_ID, rodaq_interface{this, &kCanonicalVtable});
    if (!result.ok()) {
        return result;
    }
    result =
        registry.register_entry(RODAQ_RUNTIME_PROJECTION_SERVICE_ID, projections_.abi_interface());
    if (!result.ok()) {
        return result;
    }
    if (!ingress_clock_correlation_.has_value()) {
        return activation_status(StatusCode::InvalidState, "DAQ ingress Clock is unavailable");
    }
    result = registry.register_entry(
        RODAQ_RUNTIME_CLOCK_CORRELATION_SERVICE_ID, ingress_clock_correlation_->abi_interface());
    if (!result.ok()) {
        return result;
    }
    return registry.register_entry(RODAQ_RUNTIME_STORAGE_SERVICE_ID, storage_service_interface());
}

rodaq_interface ActivationState::acquisition_service_interface(std::size_t plugin_index) noexcept {
    return rodaq_interface{
        acquisition_contexts_[plugin_index].get(),
        &kAcquisitionVtable,
    };
}

rodaq_interface ActivationState::storage_service_interface() noexcept {
    return rodaq_interface{
        storage_context_.get(),
        &kStorageVtable,
    };
}

}  // namespace rodaq
