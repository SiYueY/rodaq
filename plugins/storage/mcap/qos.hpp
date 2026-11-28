#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <rodaq/c/canonical.h>
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
constexpr std::uint32_t kMaxQosProfiles = 256;
constexpr std::size_t kMaxRos2MetadataBytes = 128 * 1024;
inline constexpr std::string_view kOfferedQosKey = "offered_qos_profiles";
inline constexpr std::string_view kTypeHashKey = "topic_type_hash";
inline constexpr std::string_view kRos2MetadataFormat = "ros2/1.0";

struct Ros2ChannelMetadata {
    std::string offered_qos_yaml{"[]\n"};
    std::string type_description_hash;
};

// Strictly parses ros2/1.0 Descriptor metadata JSON (or empty metadata) into
// rosbag2-compatible channel metadata. Ignores optional diagnostic subscription block;
// fails closed on unknown format, duplicate keys, or invalid QoS/type-hash fields.
Result<Ros2ChannelMetadata> parse_ros2_metadata(const rodaq_descriptor_metadata_t& metadata);

}  // namespace rodaq::mcap_storage
