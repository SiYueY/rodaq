#ifndef RODAQ_PLUGIN_PLUGIN_CATALOG_HPP_
#define RODAQ_PLUGIN_PLUGIN_CATALOG_HPP_

#include "plugin/plugin_library.hpp"

#include <rodaq/result.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rodaq::plugin {

// Catalog mutations are Control Plane serialized; leases remain independently copyable.
class PluginCatalog final {
public:
    PluginCatalog() = default;

    PluginCatalog(const PluginCatalog&) = delete;
    PluginCatalog& operator=(const PluginCatalog&) = delete;

    Result<PluginLibraryLease> register_library(PluginLibrary library);
    Result<PluginLibraryLease> find(std::string_view plugin_id) const;
    Result<PluginLibraryLease> find_by_path(const std::filesystem::path& canonical_path) const;
    Result<void> remove(std::string_view plugin_id);

private:
    std::unordered_map<std::string, PluginLibrary> libraries_;
    std::unordered_map<std::string, std::string> plugin_id_by_path_;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_CATALOG_HPP_
