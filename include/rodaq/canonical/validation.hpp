#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>
#include <tuple>

#include <rodaq/canonical/c/ros2.h>
#include <rodaq/canonical/c/types.h>
#include <rodaq/plugin/abi.hpp>

namespace rodaq::canonical {

/** Validate semantic fields explicitly; never compare compiler padding bytes. */
inline bool valid_source_id(const rodaq_source_id_t& source, bool present) noexcept {
    if (!std::all_of(std::begin(source.reserved), std::end(source.reserved), [](auto byte) {
            return byte == 0;
        })) {
        return false;
    }
    if (!present) {
        return source.kind == RODAQ_SOURCE_ID_INVALID && source.size == 0 &&
               std::all_of(std::begin(source.data), std::end(source.data), [](auto byte) {
                   return byte == 0;
               });
    }
    if ((source.kind != RODAQ_SOURCE_ID_ROS2_GID && source.kind != RODAQ_SOURCE_ID_RTPS_GUID) ||
        source.size == 0 || source.size > RODAQ_SOURCE_ID_DATA_CAPACITY ||
        (source.kind == RODAQ_SOURCE_ID_RTPS_GUID && source.size != 16)) {
        return false;
    }
    return std::all_of(std::begin(source.data) + source.size, std::end(source.data), [](auto byte) {
        return byte == 0;
    });
}

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

/** Canonical comparison key for ROS 2 QoS profiles; excludes padding/reserved bytes. */
inline auto qos_key(const rodaq_ros2_qos_profile_t& qos) noexcept {
    return std::make_tuple(
        qos.history, qos.reliability, qos.durability, qos.liveliness, qos.depth, qos.deadline.sec,
        qos.deadline.nsec, qos.lifespan.sec, qos.lifespan.nsec, qos.liveliness_lease_duration.sec,
        qos.liveliness_lease_duration.nsec, qos.avoid_ros_namespace_conventions);
}

inline bool valid_text(rodaq_string_t text, std::uint32_t limit, bool token = false) noexcept {
    if (text.size == 0 || text.size > limit || !plugin::abi::valid_string(text) ||
        std::memchr(text.data, 0, text.size) != nullptr) {
        return false;
    }
    return !token || std::all_of(text.data, text.data + text.size, [](unsigned char byte) {
        return byte >= 0x21 && byte <= 0x7e;
    });
}

template <typename Descriptor>
bool valid_descriptor_fields(const Descriptor& descriptor) noexcept {
    if (!valid_text(descriptor.channel_name, 1024) ||
        !valid_text(descriptor.message_encoding, 64, true) ||
        (descriptor.schema_name.size != 0 && !valid_text(descriptor.schema_name, 1024)) ||
        (descriptor.schema_encoding.size != 0 &&
         !valid_text(descriptor.schema_encoding, 64, true)) ||
        (descriptor.schema.size != 0 && descriptor.schema.data == nullptr) ||
        ((descriptor.ros2_offered_qos_profile_count == 0) !=
         (descriptor.ros2_offered_qos_profiles == nullptr))) {
        return false;
    }
    if (descriptor.data_profile == RODAQ_DATA_PROFILE_GENERIC) {
        return descriptor.ros2_type_description_hash.size == 0 &&
               descriptor.ros2_offered_qos_profile_count == 0;
    }
    if (descriptor.data_profile != RODAQ_DATA_PROFILE_ROS2 || descriptor.schema.size == 0 ||
        descriptor.schema_name.size == 0 ||
        std::string_view(descriptor.message_encoding.data, descriptor.message_encoding.size) !=
            "cdr") {
        return false;
    }
    const std::string_view encoding(
        descriptor.schema_encoding.data, descriptor.schema_encoding.size);
    if (encoding != "ros2msg" && encoding != "ros2idl") {
        return false;
    }
    if (descriptor.ros2_type_description_hash.size != 0 &&
        !valid_text(descriptor.ros2_type_description_hash, 128, true)) {
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(descriptor.ros2_offered_qos_profiles) %
            alignof(rodaq_ros2_qos_profile_t) !=
        0) {
        return false;
    }
    for (std::uint32_t i = 0; i < descriptor.ros2_offered_qos_profile_count; ++i) {
        if (!canonical::valid_qos(descriptor.ros2_offered_qos_profiles[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace rodaq::canonical
