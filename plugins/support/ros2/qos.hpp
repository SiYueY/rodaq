#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <tuple>
#include <vector>

#include <rodaq/result.hpp>

#include "ros2/ros2.h"

namespace rodaq::ros2 {

struct RequestedQos {
    rodaq_ros2_qos_reliability_t reliability{RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT};
    rodaq_ros2_qos_durability_t durability{RODAQ_ROS2_QOS_DURABILITY_VOLATILE};
    std::uint64_t depth{10};
};

inline bool valid_qos(const rodaq_ros2_qos_profile_t& qos) noexcept {
    return qos.history <= RODAQ_ROS2_QOS_HISTORY_UNKNOWN &&
           qos.reliability <= RODAQ_ROS2_QOS_RELIABILITY_UNKNOWN &&
           qos.durability <= RODAQ_ROS2_QOS_DURABILITY_UNKNOWN &&
           qos.liveliness <= RODAQ_ROS2_QOS_LIVELINESS_UNKNOWN && qos.deadline.nsec < 1000000000 &&
           qos.lifespan.nsec < 1000000000 && qos.liveliness_lease_duration.nsec < 1000000000 &&
           qos.avoid_ros_namespace_conventions <= 1 &&
           std::all_of(std::begin(qos.reserved), std::end(qos.reserved), [](auto byte) {
               return byte == 0;
           });
}

inline auto qos_key(const rodaq_ros2_qos_profile_t& qos) noexcept {
    return std::make_tuple(
        qos.history, qos.reliability, qos.durability, qos.liveliness, qos.depth, qos.deadline.sec,
        qos.deadline.nsec, qos.lifespan.sec, qos.lifespan.nsec, qos.liveliness_lease_duration.sec,
        qos.liveliness_lease_duration.nsec, qos.avoid_ros_namespace_conventions);
}

Result<std::vector<rodaq_ros2_qos_profile_t>> normalize_qos(
    std::vector<rodaq_ros2_qos_profile_t> profiles);
RequestedQos adapt_qos(const std::vector<rodaq_ros2_qos_profile_t>& offers, std::uint64_t depth);
bool qos_compatible(
    const RequestedQos& installed, const std::vector<rodaq_ros2_qos_profile_t>& offers);

}  // namespace rodaq::ros2
