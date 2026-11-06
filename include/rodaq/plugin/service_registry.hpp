#ifndef RODAQ_PLUGIN_SERVICE_REGISTRY_HPP_
#define RODAQ_PLUGIN_SERVICE_REGISTRY_HPP_

#include <rodaq/plugin/abi/service.h>
#include <rodaq/plugin/abi_support.hpp>

#include <cstddef>
#include <string_view>

namespace rodaq::plugin {

class InterfaceView final {
public:
    InterfaceView() = default;
    explicit InterfaceView(rodaq_interface interface) noexcept;

    void* context() const noexcept;
    const void* v_table() const noexcept;
    bool valid() const noexcept;
    rodaq_interface abi() const noexcept;

private:
    rodaq_interface interface_{};
};

class ServiceRegistryView final {
public:
    ServiceRegistryView() = default;

    static Result<ServiceRegistryView> from_abi(const rodaq_service_registry& registry);
    Result<InterfaceView> query(std::string_view service_id) const;

private:
    explicit ServiceRegistryView(const rodaq_service_registry& registry) noexcept;

    rodaq_service_registry registry_{};
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_SERVICE_REGISTRY_HPP_
