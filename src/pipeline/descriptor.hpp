#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/acquisition/types.hpp>
#include <rodaq/storage/types.hpp>
#include "../ros2_subscription.hpp"

namespace rodaq::pipeline {

struct DescriptorRegistryState;
class DescriptorRegistry;

// The lease is destroyed after owned facts, keeping accounting/backing until
// actual descriptor access ends. It has no dependency on the registry facade.
class DescriptorReservation {
public:
    DescriptorReservation(const DescriptorReservation&) = delete;
    DescriptorReservation& operator=(const DescriptorReservation&) = delete;
    DescriptorReservation(DescriptorReservation&& other) noexcept;
    DescriptorReservation& operator=(DescriptorReservation&&) = delete;
    ~DescriptorReservation() noexcept;

private:
    friend class DescriptorRegistry;
    DescriptorReservation(
        std::shared_ptr<DescriptorRegistryState> state, std::size_t stream_index,
        std::size_t slot_index, std::size_t bytes) noexcept;
    std::shared_ptr<DescriptorRegistryState> state_;
    std::size_t stream_index_;
    std::size_t slot_index_;
    std::size_t bytes_;
};

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
    // Private normalized-fact fingerprint; never used as persistent ID.
    std::string fingerprint;
};

class Descriptor {
private:
    // First member is destroyed last, after all variable-size facts.
    DescriptorReservation reservation_;
    friend class DescriptorRegistry;
    Descriptor(
        DescriptorReservation reservation, rodaq_stream_id_t stream, rodaq_descriptor_id_t identity,
        acquisition::DescriptorToken token, DescriptorFacts facts) noexcept;

public:
    const rodaq_stream_id_t stream_id;
    const rodaq_descriptor_id_t descriptor_id;
    const acquisition::DescriptorToken token;
    const DescriptorFacts facts;

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&&) = delete;
    Descriptor& operator=(Descriptor&&) = delete;
    ~Descriptor() = default;

    // All views remain valid while this immutable owner remains referenced,
    // including beyond an observer timeout until actual Storage call return.
    storage::StreamDescriptor view() const noexcept;
};

using DescriptorRef = std::shared_ptr<const Descriptor>;

}  // namespace rodaq::pipeline
