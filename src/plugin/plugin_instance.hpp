#ifndef RODAQ_PLUGIN_PLUGIN_INSTANCE_HPP_
#define RODAQ_PLUGIN_PLUGIN_INSTANCE_HPP_

#include "plugin/plugin_library.hpp"

#include <memory>

namespace rodaq::plugin {

class PluginInstanceState;

class PluginInstanceLease final {
public:
    PluginInstanceLease() = default;
    PluginInstanceLease(const PluginInstanceLease&) = default;
    PluginInstanceLease(PluginInstanceLease&&) noexcept = default;
    PluginInstanceLease& operator=(const PluginInstanceLease&) = default;
    PluginInstanceLease& operator=(PluginInstanceLease&&) noexcept = default;
    ~PluginInstanceLease() = default;

    bool valid() const noexcept;
    void quarantine() noexcept;

private:
    explicit PluginInstanceLease(std::shared_ptr<PluginInstanceState> state) noexcept;

    std::shared_ptr<PluginInstanceState> state_;

    friend class AcquisitionPluginInstance;
    friend class StoragePluginInstance;
};

class PluginInstanceStateAccess {
public:
    using BindFunction =
        rodaq_status (*)(void*, const rodaq_service_registry*, rodaq_error*) noexcept;
    using ConfigureFunction = rodaq_status (*)(void*, rodaq_config_view, rodaq_error*) noexcept;

    static std::shared_ptr<PluginInstanceState> create(
        const PluginLibraryLease& library_lease, void* instance, void (*destroy)(void*) noexcept,
        BindFunction bind_function, ConfigureFunction configure_function);

    static Result<void> bind(
        const std::shared_ptr<PluginInstanceState>& state, const rodaq_service_registry& services);

    static Result<void> configure(
        const std::shared_ptr<PluginInstanceState>& state, rodaq_config_view config);
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_INSTANCE_HPP_
