#ifndef RODAQ_CONFIG_VALIDATION_HPP_
#define RODAQ_CONFIG_VALIDATION_HPP_

#include <rodaq/daq_config.hpp>
#include <rodaq/result.hpp>

namespace rodaq {

Result<void> validate_daq_config(const DAQConfig& config);

}  // namespace rodaq

#endif  // RODAQ_CONFIG_VALIDATION_HPP_
