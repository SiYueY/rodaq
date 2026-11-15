#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <cstring>
#include <string_view>
#include <rodaq/plugin/abi.hpp>

#include <rodaq/canonical/c/ros2.h>
#include <rodaq/canonical/c/types.h>

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

inline bool valid_timestamp(const rodaq_record_timestamp_t& value, bool present) noexcept {
    if (!present) {
        return value.role == 0 && value.clock_domain == 0 && value.clock_id == 0 &&
               value.value == 0 && value.flags == 0 && value.reserved == 0;
    }
    return value.role == RODAQ_TIMESTAMP_ROLE_SOURCE_PUBLICATION && value.flags == 0 &&
           value.reserved == 0 &&
           ((value.clock_domain == RODAQ_CLOCK_DOMAIN_UNIX_EPOCH && value.clock_id == 0) ||
            (value.clock_domain == RODAQ_CLOCK_DOMAIN_SOURCE_NATIVE && value.clock_id != 0));
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

inline bool valid_source_facts(
    const rodaq_source_id_t& source, rodaq_source_session_id_t session, std::uint64_t sequence,
    rodaq_sequence_scope_t scope, std::uint8_t has_source, std::uint8_t has_session,
    std::uint8_t has_sequence) noexcept {
    if ((scope == RODAQ_SEQUENCE_SCOPE_SOURCE && !has_source) ||
        (scope == RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION && (!has_source || !has_session))) {
        return false;
    }
    return has_source <= 1 && has_session <= 1 && has_sequence <= 1 &&
           valid_source_id(source, has_source != 0) &&
           (has_session ? session != 0 : session == 0) &&
           (has_sequence ? scope >= RODAQ_SEQUENCE_SCOPE_OPAQUE &&
                               scope <= RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION
                         : sequence == 0 && scope == RODAQ_SEQUENCE_SCOPE_INVALID);
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
bool valid_descriptor_fields(const Descriptor& proposal) noexcept {
    if (!valid_text(proposal.channel_name, 1024) ||
        !valid_text(proposal.message_encoding, 64, true) ||
        (proposal.schema_name.size != 0 && !valid_text(proposal.schema_name, 1024)) ||
        (proposal.schema_encoding.size != 0 && !valid_text(proposal.schema_encoding, 64, true)) ||
        (proposal.schema.size != 0 && proposal.schema.data == nullptr) ||
        ((proposal.ros2_offered_qos_profile_count == 0) !=
         (proposal.ros2_offered_qos_profiles == nullptr))) {
        return false;
    }
    if (proposal.data_profile == RODAQ_DATA_PROFILE_GENERIC) {
        return proposal.ros2_type_description_hash.size == 0 &&
               proposal.ros2_offered_qos_profile_count == 0;
    }
    if (proposal.data_profile != RODAQ_DATA_PROFILE_ROS2 || proposal.schema.size == 0 ||
        proposal.schema_name.size == 0 ||
        std::string_view(proposal.message_encoding.data, proposal.message_encoding.size) != "cdr") {
        return false;
    }
    const std::string_view encoding(proposal.schema_encoding.data, proposal.schema_encoding.size);
    if (encoding != "ros2msg" && encoding != "ros2idl") {
        return false;
    }
    if (proposal.ros2_type_description_hash.size != 0 &&
        !valid_text(proposal.ros2_type_description_hash, 128, true)) {
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(proposal.ros2_offered_qos_profiles) %
            alignof(rodaq_ros2_qos_profile_t) !=
        0) {
        return false;
    }
    for (std::uint32_t i = 0; i < proposal.ros2_offered_qos_profile_count; ++i) {
        if (!canonical::valid_qos(proposal.ros2_offered_qos_profiles[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace rodaq::canonical
