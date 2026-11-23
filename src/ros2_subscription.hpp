#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <rodaq/acquisition/c/types.h>

#include "persistence/json.hpp"

namespace rodaq::canonical {

// Immutable Descriptor/Manifest-owned observation. No Native pointers survive
// the actual proposal return, and the existing Descriptor quota owns its bytes.
struct Ros2SubscriptionFacts {
    std::string ros_distribution;
    std::string rmw_implementation;
    rodaq_ros2_qos_profile_t installed_requested_qos{};
    std::uint64_t subscription_generation{0};
    std::uint32_t executor_kind{0};
    std::uint32_t executor_threads{0};

    rodaq_ros2_subscription_facts_t view() const noexcept;
    std::size_t data_bytes() const noexcept;
};

Ros2SubscriptionFacts copy_subscription_facts(const rodaq_ros2_subscription_facts_t& source);
persistence::Json subscription_document(const Ros2SubscriptionFacts& facts);
persistence::Json qos_document(const rodaq_ros2_qos_profile_t& qos);

}  // namespace rodaq::canonical
