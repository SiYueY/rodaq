#ifndef RODAQ_CLOCK_INGRESS_TIME_HPP_
#define RODAQ_CLOCK_INGRESS_TIME_HPP_

#include "activation/activation_error.hpp"

#include <rodaq/result.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

namespace rodaq {

inline constexpr std::string_view kIngressTimestampRole = "rodaq/acquisition_ingress";

inline Result<std::int64_t> capture_ingress_reading() {
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (count > std::numeric_limits<std::int64_t>::max()) {
        return activation_status(StatusCode::InternalError, "DAQ ingress clock reading overflowed");
    }
    return static_cast<std::int64_t>(count);
}

}  // namespace rodaq

#endif  // RODAQ_CLOCK_INGRESS_TIME_HPP_
