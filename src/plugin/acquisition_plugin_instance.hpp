#ifndef RODAQ_PLUGIN_ACQUISITION_PLUGIN_INSTANCE_HPP_
#define RODAQ_PLUGIN_ACQUISITION_PLUGIN_INSTANCE_HPP_

#include "plugin/plugin_instance.hpp"
#include "plugin/plugin_library.hpp"

#include <rodaq/result.hpp>

#include <memory>
#include <string_view>

namespace rodaq::plugin {

class AcquisitionPluginInstance final {
public:
    static Result<AcquisitionPluginInstance> create(PluginLibraryLease library_lease);

    AcquisitionPluginInstance(AcquisitionPluginInstance&&) noexcept = default;
    AcquisitionPluginInstance& operator=(AcquisitionPluginInstance&&) noexcept = default;

    AcquisitionPluginInstance(const AcquisitionPluginInstance&) = delete;
    AcquisitionPluginInstance& operator=(const AcquisitionPluginInstance&) = delete;

    ~AcquisitionPluginInstance() = default;

    Result<void> bind(const rodaq_service_registry& services);
    Result<void> configure(rodaq_config_view config);
    Result<rodaq_interface> query_interface(std::string_view interface_id);
    Result<rodaq_interface> required_control_interface() const;

    PluginInstanceLease lease() const noexcept;
    bool valid() const noexcept;

private:
    explicit AcquisitionPluginInstance(std::shared_ptr<PluginInstanceState> state) noexcept;

    std::shared_ptr<PluginInstanceState> state_;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_ACQUISITION_PLUGIN_INSTANCE_HPP_
