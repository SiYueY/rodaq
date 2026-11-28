#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/acquisition/types.hpp>
#include <rodaq/result.hpp>
#include <rodaq/storage/types.hpp>

#include "../ros2_subscription.hpp"

namespace rodaq::pipeline {

struct DescriptorFacts {
    rodaq_data_profile_t data_profile{RODAQ_DATA_PROFILE_INVALID};
    std::string channel_name;
    std::string message_encoding;
    std::string schema_name;
    std::string schema_encoding;
    std::vector<std::uint8_t> schema;
    std::string ros2_type_description_hash;
    std::vector<rodaq_ros2_qos_profile_t> offered_qos;
    std::string schema_digest;
    std::optional<canonical::Ros2SubscriptionFacts> ros2_subscription;
};

// Immutable bind-once Stream Descriptor owned by a Recording Instance and
// retained by accepted Records through actual Storage return.
class Descriptor {
public:
    static Result<std::shared_ptr<const Descriptor>> create(
        const acquisition::Descriptor& proposal, rodaq_descriptor_id_t descriptor_id,
        std::uint32_t max_schema_bytes = 1024 * 1024, std::uint32_t max_qos_profiles = 256);

    const rodaq_stream_id_t stream_id;
    const rodaq_descriptor_id_t descriptor_id;
    const DescriptorFacts facts;

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    ~Descriptor() = default;

    // All views remain valid while this immutable owner remains referenced,
    // including beyond an observer timeout until actual Storage call return.
    storage::StreamDescriptor view() const noexcept;

private:
    Descriptor(
        rodaq_stream_id_t stream, rodaq_descriptor_id_t identity, DescriptorFacts owned) noexcept;
};

using DescriptorRef = std::shared_ptr<const Descriptor>;

}  // namespace rodaq::pipeline
