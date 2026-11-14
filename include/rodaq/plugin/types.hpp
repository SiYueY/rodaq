#pragma once

#include <cstdint>
#include <string>

namespace rodaq::plugin {

using PluginInstanceId = std::uint64_t;

enum class PluginManagerPhase { Running, ShuttingDown, Degraded, Stopped };

enum class PluginInstancePhase { Initializing, Open, Closing, Closed, Unresolved };

struct PluginInfo {
    std::string plugin_id;
    std::string plugin_name;
    std::string plugin_version;
};

struct PluginBinaryInfo {
    std::string plugin_id;
    std::string plugin_version;
    std::string sha256;
};

}  // namespace rodaq::plugin
