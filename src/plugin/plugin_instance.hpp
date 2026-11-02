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
    static std::shared_ptr<PluginInstanceState> create(
        const PluginLibraryLease& library_lease, void* instance, void (*destroy)(void*) noexcept);
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_INSTANCE_HPP_
