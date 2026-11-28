#pragma once

#include <cstdint>
#include <string>

#include <rodaq/plugin/c/types.h>

namespace rodaq::plugin {

using PluginInstanceId = std::uint64_t;

enum class PluginManagerPhase { Running, ShuttingDown, Degraded, Stopped };

enum class PluginInstancePhase { Initializing, Open, Closing, Closed, Unresolved };

enum class LogLevel : std::uint32_t {
    Trace = RODAQ_LOG_TRACE,
    Debug = RODAQ_LOG_DEBUG,
    Info = RODAQ_LOG_INFO,
    Warning = RODAQ_LOG_WARNING,
    Error = RODAQ_LOG_ERROR,
    Critical = RODAQ_LOG_CRITICAL
};

struct PluginInfo {
    std::string plugin_id;
    std::string plugin_name;
    std::string plugin_version;
};

}  // namespace rodaq::plugin
