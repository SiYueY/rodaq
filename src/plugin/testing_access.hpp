#pragma once

#include "lifecycle_policy.hpp"

#include <rodaq/plugin/manager.hpp>

namespace rodaq::plugin {

class PluginManagerTestingAccess final {
public:
    [[nodiscard]] static Result<PluginManager> create(
        PluginManagerOptions options, LifecyclePolicy lifecycle_policy);
};

}  // namespace rodaq::plugin
