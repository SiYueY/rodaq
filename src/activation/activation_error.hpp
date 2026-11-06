#ifndef RODAQ_ACTIVATION_ACTIVATION_ERROR_HPP_
#define RODAQ_ACTIVATION_ACTIVATION_ERROR_HPP_

#include <rodaq/status.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace rodaq {

inline constexpr std::size_t kMaxSecondaryFailures = 8;

inline Status activation_status(StatusCode status_code, std::string message) {
    return Status(status_code, "activation", 0, std::move(message));
}

}  // namespace rodaq

#endif  // RODAQ_ACTIVATION_ACTIVATION_ERROR_HPP_
