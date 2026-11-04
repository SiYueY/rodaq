#ifndef RODAQ_PLUGIN_STORAGE_PLUGIN_INSTANCE_HPP_
#define RODAQ_PLUGIN_STORAGE_PLUGIN_INSTANCE_HPP_

#include "plugin/plugin_instance.hpp"
#include "plugin/plugin_library.hpp"

#include <rodaq/result.hpp>

#include <memory>
#include <string_view>

namespace rodaq::plugin {

class StoragePluginInstance final {
public:
    static Result<StoragePluginInstance> create(PluginLibraryLease library_lease);

    StoragePluginInstance(StoragePluginInstance&&) noexcept = default;
    StoragePluginInstance& operator=(StoragePluginInstance&&) noexcept = default;

    StoragePluginInstance(const StoragePluginInstance&) = delete;
    StoragePluginInstance& operator=(const StoragePluginInstance&) = delete;

    ~StoragePluginInstance() = default;

    Result<void> bind(const rodaq_service_registry& services);
    Result<void> configure(rodaq_config_view config);
    Result<void> open(const rodaq_storage_open_request& request);
    Result<rodaq_storage_submit_result> submit(const rodaq_storage_submission& submission);
    Result<void> flush();
    Result<void> close(const rodaq_storage_close_request& request);
    void destroy() noexcept;
    Result<rodaq_interface> query_interface(std::string_view interface_id);
    Result<rodaq_interface> required_control_interface() const;

    PluginInstanceLease lease() const noexcept;
    bool valid() const noexcept;

private:
    explicit StoragePluginInstance(std::shared_ptr<PluginInstanceState> state) noexcept;

    std::shared_ptr<PluginInstanceState> state_;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_STORAGE_PLUGIN_INSTANCE_HPP_
