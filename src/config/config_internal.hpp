#ifndef RODAQ_CONFIG_CONFIG_INTERNAL_HPP_
#define RODAQ_CONFIG_CONFIG_INTERNAL_HPP_

#include <rodaq/config/config.hpp>
#include <rodaq/plugin/abi/config.h>

namespace rodaq {

rodaq_config_view config_abi_view(const ConfigNodeView& view) noexcept;

}  // namespace rodaq

#endif  // RODAQ_CONFIG_CONFIG_INTERNAL_HPP_
