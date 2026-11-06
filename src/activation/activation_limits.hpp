#ifndef RODAQ_ACTIVATION_ACTIVATION_LIMITS_HPP_
#define RODAQ_ACTIVATION_ACTIVATION_LIMITS_HPP_

#include <chrono>
#include <cstddef>

namespace rodaq {

struct ActivationLimits {
    std::size_t maximum_payload_bytes = 0;
    std::size_t dispatch_capacity = 0;
    std::size_t operation_capacity = 0;
    std::size_t completion_capacity = 0;

    std::chrono::nanoseconds storage_handoff_timeout{};
    std::chrono::nanoseconds descriptor_progress_timeout{};
    std::chrono::nanoseconds binding_progress_timeout{};
    std::chrono::nanoseconds storage_flush_timeout{};
    std::chrono::nanoseconds storage_close_timeout{};
};

}  // namespace rodaq

#endif  // RODAQ_ACTIVATION_ACTIVATION_LIMITS_HPP_
