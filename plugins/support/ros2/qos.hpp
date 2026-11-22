#pragma once

#include <cstdint>
#include <vector>

#include <rodaq/canonical/c/ros2.h>
#include <rodaq/result.hpp>

namespace rodaq::ros2 {

struct RequestedQos {
    rodaq_ros2_qos_reliability_t reliability{RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT};
    rodaq_ros2_qos_durability_t durability{RODAQ_ROS2_QOS_DURABILITY_VOLATILE};
    std::uint64_t depth{10};
};

Result<std::vector<rodaq_ros2_qos_profile_t>> normalize_qos(
    std::vector<rodaq_ros2_qos_profile_t> profiles);
RequestedQos adapt_qos(const std::vector<rodaq_ros2_qos_profile_t>& offers, std::uint64_t depth);
bool qos_compatible(
    const RequestedQos& installed, const std::vector<rodaq_ros2_qos_profile_t>& offers);

}  // namespace rodaq::ros2
