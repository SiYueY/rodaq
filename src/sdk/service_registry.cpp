#include <rodaq/plugin/service_registry.hpp>

namespace rodaq {

InterfaceView::InterfaceView(rodaq_interface interface) noexcept : interface_(interface) {}
void* InterfaceView::context() const noexcept { return interface_.context; }
const void* InterfaceView::v_table() const noexcept { return interface_.vtable; }
bool InterfaceView::valid() const noexcept {
    return interface_.context != nullptr && interface_.vtable != nullptr;
}
rodaq_interface InterfaceView::abi() const noexcept { return interface_; }

ServiceRegistryView::ServiceRegistryView(const rodaq_service_registry& registry) noexcept
: registry_(registry) {}

Result<ServiceRegistryView> ServiceRegistryView::from_abi(const rodaq_service_registry& registry) {
    if (registry.context == nullptr || registry.vtable == nullptr ||
        registry.vtable->protocol_version != RODAQ_SERVICE_REGISTRY_PROTOCOL_VERSION ||
        registry.vtable->struct_size < RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE ||
        registry.vtable->query == nullptr) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "sdk", 0,
            "Service Registry ABI is incomplete or incompatible"));
    }
    return ServiceRegistryView(registry);
}
Result<InterfaceView> ServiceRegistryView::query(std::string_view service_id) const {
    if (registry_.context == nullptr || registry_.vtable == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "sdk", 0, "ServiceRegistryView is invalid"));
    }
    rodaq_interface interface {};
    rodaq_error error{};
    const rodaq_status status = registry_.vtable->query(
        registry_.context, rodaq_string_view{service_id.data(), service_id.size()}, &interface,
        &error);
    if (status != RODAQ_STATUS_OK) {
        return nonstd::make_unexpected(result_from_status(status, &error, "sdk").error());
    }
    if (interface.context == nullptr || interface.vtable == nullptr) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "sdk", 0,
            "Service Registry returned an incomplete interface"));
    }
    return InterfaceView(interface);
}

}  // namespace rodaq
