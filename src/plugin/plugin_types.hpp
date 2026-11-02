#ifndef RODAQ_PLUGIN_PLUGIN_TYPES_HPP_
#define RODAQ_PLUGIN_PLUGIN_TYPES_HPP_

#include <string>

namespace rodaq::plugin {

enum class PluginFamily {
    Acquisition,
    Storage,
};

struct PluginMetadata {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
};

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_TYPES_HPP_
