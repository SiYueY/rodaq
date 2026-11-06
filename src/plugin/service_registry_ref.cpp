#include <rodaq/plugin/service_registry.hpp>

namespace rodaq::plugin {

InterfaceRef::InterfaceRef(rodaq_interface interface) noexcept : interface_(interface) {}
void* InterfaceRef::context() const noexcept { return interface_.context; }
const void* InterfaceRef::v_table() const noexcept { return interface_.vtable; }
bool InterfaceRef::valid() const noexcept {
    return interface_.context != nullptr && interface_.vtable != nullptr;
}
rodaq_interface InterfaceRef::abi() const noexcept { return interface_; }

ServiceRegistryRef::ServiceRegistryRef(const rodaq_service_registry& registry) noexcept
: registry_(registry) {}

Result<ServiceRegistryRef> ServiceRegistryRef::from_abi(const rodaq_service_registry& registry) {
    if (registry.context == nullptr || registry.vtable == nullptr ||
        registry.vtable->protocol_version != RODAQ_SERVICE_REGISTRY_PROTOCOL_VERSION ||
        registry.vtable->struct_size < RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE ||
        registry.vtable->query == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0,
            "Service Registry ABI is incomplete or incompatible");
    }
    return ServiceRegistryRef(registry);
}
Result<InterfaceRef> ServiceRegistryRef::query(std::string_view service_id) const {
    if (registry_.context == nullptr || registry_.vtable == nullptr) {
        return rodaq::Status(StatusCode::InvalidState, "sdk", 0, "ServiceRegistryRef is invalid");
    }
    rodaq_interface interface {};
    rodaq_status error{};
    const rodaq_status_code status = registry_.vtable->query(
        registry_.context, rodaq_string_view{service_id.data(), service_id.size()}, &interface,
        &error);
    if (status != RODAQ_STATUS_OK) {
        return status_from_abi(status, &error, "sdk");
    }
    if (interface.context == nullptr || interface.vtable == nullptr) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "sdk", 0,
            "Service Registry returned an incomplete interface");
    }
    return InterfaceRef(interface);
}

}  // namespace rodaq::plugin
