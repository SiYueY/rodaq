#ifndef RODAQ_PLUGIN_PLUGIN_VALIDATION_HPP_
#define RODAQ_PLUGIN_PLUGIN_VALIDATION_HPP_

#include "plugin/plugin_types.hpp"

#include <rodaq/plugin/abi/acquisition.h>
#include <rodaq/plugin/abi/plugin.h>
#include <rodaq/plugin/abi/storage.h>
#include <rodaq/result.hpp>

#include <string_view>

namespace rodaq::plugin {

bool is_valid_plugin_id(std::string_view value) noexcept;
bool is_valid_semantic_version(std::string_view value) noexcept;
bool is_valid_utf8_without_nul(std::string_view value) noexcept;

Result<PluginMetadata> validate_plugin_descriptor(const rodaq_plugin_descriptor& descriptor);

Result<void> validate_acquisition_protocol(const rodaq_acquisition_plugin& protocol);
Result<void> validate_storage_protocol(const rodaq_storage_plugin& protocol);

Result<rodaq_interface> validate_acquisition_control_interface(rodaq_interface interface);
Result<rodaq_interface> validate_storage_control_interface(rodaq_interface interface);

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_VALIDATION_HPP_
