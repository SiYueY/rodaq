#ifndef RODAQ_PLUGIN_SERVICE_REGISTRY_HPP_
#define RODAQ_PLUGIN_SERVICE_REGISTRY_HPP_

#include <rodaq/plugin/abi/service.h>
#include <rodaq/plugin/abi_support.hpp>

#include <cstddef>
#include <string_view>

namespace rodaq {

class InterfaceView final {
public:
    InterfaceView() = default;
    explicit InterfaceView(rodaq_interface interface) noexcept : interface_(interface) {}

    void* Context() const noexcept { return interface_.context; }
    const void* VTable() const noexcept { return interface_.vtable; }
    bool Valid() const noexcept {
        return interface_.context != nullptr && interface_.vtable != nullptr;
    }
    rodaq_interface Abi() const noexcept { return interface_; }

private:
    rodaq_interface interface_{};
};

class ServiceRegistryView final {
public:
    ServiceRegistryView() = default;

    static Result<ServiceRegistryView> FromAbi(const rodaq_service_registry& registry) {
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

    Result<InterfaceView> Query(std::string_view service_id) const {
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

private:
    explicit ServiceRegistryView(const rodaq_service_registry& registry) noexcept
    : registry_(registry) {}

    rodaq_service_registry registry_{};
};

}  // namespace rodaq

#endif  // RODAQ_PLUGIN_SERVICE_REGISTRY_HPP_
