#ifndef RODAQ_PLUGIN_PLUGIN_STATUS_HPP_
#define RODAQ_PLUGIN_PLUGIN_STATUS_HPP_

#include <rodaq/status.hpp>
#include <rodaq/plugin/abi_support.hpp>
#include <rodaq/result.hpp>

#include <string>
#include <string_view>

namespace rodaq::plugin {

Status invalid_argument_status(std::string message);
Status internal_status(std::string message);

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_STATUS_HPP_
