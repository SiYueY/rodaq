#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <rodaq/canonical/c/ros2.h>
#include <rodaq/result.hpp>

namespace rodaq::mcap_storage {

inline constexpr char kQosFormat[] =
    "- history: {}\n"
    "  depth: {}\n"
    "  reliability: {}\n"
    "  durability: {}\n"
    "  deadline:\n"
    "    sec: {}\n"
    "    nsec: {}\n"
    "  lifespan:\n"
    "    sec: {}\n"
    "    nsec: {}\n"
    "  liveliness: {}\n"
    "  liveliness_lease_duration:\n"
    "    sec: {}\n"
    "    nsec: {}\n"
    "  avoid_ros_namespace_conventions: {}\n";
// Twelve replacements: four single-digit policies, uint64 depth, three
// normalized sec/nsec pairs and a boolean. Includes exactly one final LF.
constexpr std::size_t kMaxEncodedQosBytes =
    sizeof(kQosFormat) - 1 - 12 * 2 + 4 + 20 + 3 * (20 + 9) + 5;
constexpr std::uint32_t kMaxQosProfiles = 1024;
inline constexpr std::string_view kOfferedQosKey = "offered_qos_profiles";
inline constexpr std::string_view kTypeHashKey = "topic_type_hash";

// Input order is already Core-canonical; preserve it exactly.
Result<std::string> encode_qos(const rodaq_ros2_qos_profile_t* profiles, std::uint32_t count);

}  // namespace rodaq::mcap_storage
