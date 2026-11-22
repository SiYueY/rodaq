#include "qos.hpp"

#include <algorithm>

#include <rodaq/canonical/validation.hpp>

#include "../../../src/ros2_qos.hpp"

namespace rodaq::ros2 {

Result<std::vector<rodaq_ros2_qos_profile_t>> normalize_qos(
    std::vector<rodaq_ros2_qos_profile_t> profiles) {
    if (profiles.size() > 1024) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (!std::all_of(profiles.begin(), profiles.end(), canonical::valid_qos)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::sort(profiles.begin(), profiles.end(), [](const auto& a, const auto& b) {
        return canonical::qos_key(a) < canonical::qos_key(b);
    });
    profiles.erase(
        std::unique(
            profiles.begin(), profiles.end(),
            [](const auto& a, const auto& b) {
                return canonical::qos_key(a) == canonical::qos_key(b);
            }),
        profiles.end());
    return profiles;
}

RequestedQos adapt_qos(const std::vector<rodaq_ros2_qos_profile_t>& offers, std::uint64_t depth) {
    RequestedQos requested;
    requested.depth = depth;
    if (!offers.empty() && std::all_of(offers.begin(), offers.end(), [](const auto& offer) {
            return offer.reliability == RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
        })) {
        requested.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    }
    if (!offers.empty() && std::all_of(offers.begin(), offers.end(), [](const auto& offer) {
            return offer.durability == RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL;
        })) {
        requested.durability = RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL;
    }
    return requested;
}

bool qos_compatible(
    const RequestedQos& installed, const std::vector<rodaq_ros2_qos_profile_t>& offers) {
    return std::all_of(offers.begin(), offers.end(), [&](const auto& offer) {
        return (installed.reliability == RODAQ_ROS2_QOS_RELIABILITY_BEST_EFFORT ||
                offer.reliability == RODAQ_ROS2_QOS_RELIABILITY_RELIABLE) &&
               (installed.durability == RODAQ_ROS2_QOS_DURABILITY_VOLATILE ||
                offer.durability == RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL);
    });
}

}  // namespace rodaq::ros2
