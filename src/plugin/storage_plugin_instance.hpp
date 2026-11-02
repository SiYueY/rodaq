#ifndef RODAQ_PLUGIN_STORAGE_PLUGIN_INSTANCE_HPP_
#define RODAQ_PLUGIN_STORAGE_PLUGIN_INSTANCE_HPP_

#include "plugin/plugin_instance.hpp"
#include "plugin/plugin_library.hpp"

#include <rodaq/result.hpp>

#include <memory>

namespace rodaq::plugin {

class StoragePluginInstance final {
public:
    static Result<StoragePluginInstance> create(PluginLibraryLease library_lease);

    StoragePluginInstance(StoragePluginInstance&&) noexcept = default;
    StoragePluginInstance& operator=(StoragePluginInstance&&) noexcept = default;

    StoragePluginInstance(const StoragePluginInstance&) = delete;
    StoragePluginInstance& operator=(const StoragePluginInstance&) = delete;

    ~StoragePluginInstance() = default;

    PluginInstanceLease lease() const noexcept;
    bool valid() const noexcept;

private:
    explicit StoragePluginInstance(std::shared_ptr<PluginInstanceState> state) noexcept;

    std::shared_ptr<PluginInstanceState> state_;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_STORAGE_PLUGIN_INSTANCE_HPP_
