#pragma once

#include <tuple>

#include <rodaq/canonical/c/ros2.h>

namespace rodaq::canonical {

// Native padding/reserved bytes never participate in normalized set identity.
inline auto qos_key(const rodaq_ros2_qos_profile_t& qos) noexcept {
    return std::make_tuple(
        qos.history, qos.reliability, qos.durability, qos.liveliness, qos.depth, qos.deadline.sec,
        qos.deadline.nsec, qos.lifespan.sec, qos.lifespan.nsec, qos.liveliness_lease_duration.sec,
        qos.liveliness_lease_duration.nsec, qos.avoid_ros_namespace_conventions);
}

}  // namespace rodaq::canonical
