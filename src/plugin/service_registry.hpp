#ifndef RODAQ_PLUGIN_SERVICE_REGISTRY_IMPL_HPP_
#define RODAQ_PLUGIN_SERVICE_REGISTRY_IMPL_HPP_

#include <rodaq/plugin/abi/service.h>
#include <rodaq/result.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq::plugin {

class ServiceRegistry final {
public:
    ServiceRegistry(std::size_t max_service_count, std::size_t max_service_id_bytes);

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) = delete;
    ServiceRegistry& operator=(ServiceRegistry&&) = delete;
    ~ServiceRegistry() = default;

    Result<void> register_entry(std::string_view service_id, rodaq_interface service);
    Result<void> freeze();
    Result<rodaq_interface> query(std::string_view service_id) const;

    rodaq_service_registry abi_view() noexcept;

private:
    struct Entry {
        std::string service_id;
        rodaq_interface service;
    };

    static rodaq_status query(
        void* context, rodaq_string_view service_id, rodaq_interface* out_service,
        rodaq_error* out_error) noexcept;

    static const rodaq_service_registry_vtable kRegistryVtable;

    const std::size_t max_service_count_;
    const std::size_t max_service_id_bytes_;
    mutable std::mutex mutex_;
    std::vector<Entry> services_;
    bool frozen_ = false;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_SERVICE_REGISTRY_IMPL_HPP_
