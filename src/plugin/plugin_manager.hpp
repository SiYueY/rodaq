#ifndef RODAQ_PLUGIN_PLUGIN_MANAGER_HPP_
#define RODAQ_PLUGIN_PLUGIN_MANAGER_HPP_

#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/plugin_catalog.hpp"
#include "plugin/storage_plugin_instance.hpp"

#include <rodaq/result.hpp>

#include <filesystem>
#include <string_view>

namespace rodaq::plugin {

// PluginManager operations are Control Plane serialized.
class PluginManager final {
public:
    PluginManager() = default;

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    Result<PluginLibraryLease> load_library(const std::filesystem::path& path);
    Result<PluginLibraryLease> find(std::string_view plugin_id) const;

    Result<AcquisitionPluginInstance> create_acquisition_plugin_instance(
        PluginLibraryLease library_lease);
    Result<StoragePluginInstance> create_storage_plugin_instance(PluginLibraryLease library_lease);

    Result<void> unload(std::string_view plugin_id);
    void quarantine(PluginInstanceLease instance_lease) noexcept;

private:
    PluginCatalog catalog_;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_MANAGER_HPP_
