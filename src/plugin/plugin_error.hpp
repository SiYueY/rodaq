#ifndef RODAQ_PLUGIN_PLUGIN_ERROR_HPP_
#define RODAQ_PLUGIN_PLUGIN_ERROR_HPP_

#include <rodaq/plugin/abi/common.h>
#include <rodaq/error.hpp>
#include <rodaq/result.hpp>

#include <string>
#include <string_view>

namespace rodaq::plugin {

Result<void> result_from_status(
    rodaq_status status, const rodaq_error* status_error, std::string_view fallback_domain);

Error invalid_argument_error(std::string message);
Error internal_error(std::string message);

}  // namespace rodaq::plugin

#endif  // RODAQ_PLUGIN_PLUGIN_ERROR_HPP_
