#include "ros2_subscription.hpp"

namespace rodaq::canonical {

rodaq_ros2_subscription_facts_t Ros2SubscriptionFacts::view() const noexcept {
    return {
        {sizeof(rodaq_ros2_subscription_facts_t)},
        {ros_distribution.data(), static_cast<std::uint32_t>(ros_distribution.size())},
        {rmw_implementation.data(), static_cast<std::uint32_t>(rmw_implementation.size())},
        installed_requested_qos,
        subscription_generation,
        executor_kind,
        executor_threads};
}

std::size_t Ros2SubscriptionFacts::data_bytes() const noexcept {
    return sizeof(rodaq_ros2_subscription_facts_t) + ros_distribution.size() +
           rmw_implementation.size();
}

Ros2SubscriptionFacts copy_subscription_facts(const rodaq_ros2_subscription_facts_t& source) {
    return {
        {source.ros_distribution.data, source.ros_distribution.size},
        {source.rmw_implementation.data, source.rmw_implementation.size},
        source.installed_requested_qos,
        source.subscription_generation,
        source.executor_kind,
        source.executor_threads};
}

persistence::Json qos_document(const rodaq_ros2_qos_profile_t& qos) {
    return {
        {"history", std::to_string(qos.history)},
        {"reliability", std::to_string(qos.reliability)},
        {"durability", std::to_string(qos.durability)},
        {"liveliness", std::to_string(qos.liveliness)},
        {"depth", std::to_string(qos.depth)},
        {"deadline_sec", std::to_string(qos.deadline.sec)},
        {"deadline_nsec", std::to_string(qos.deadline.nsec)},
        {"lifespan_sec", std::to_string(qos.lifespan.sec)},
        {"lifespan_nsec", std::to_string(qos.lifespan.nsec)},
        {"lease_sec", std::to_string(qos.liveliness_lease_duration.sec)},
        {"lease_nsec", std::to_string(qos.liveliness_lease_duration.nsec)},
        {"avoid_ros_namespace_conventions", std::to_string(qos.avoid_ros_namespace_conventions)}};
}

persistence::Json subscription_document(const Ros2SubscriptionFacts& facts) {
    return {
        {"ros_distribution", facts.ros_distribution},
        {"rmw_implementation", facts.rmw_implementation},
        {"installed_requested_qos", qos_document(facts.installed_requested_qos)},
        {"subscription_generation", std::to_string(facts.subscription_generation)},
        {"executor_kind", std::to_string(facts.executor_kind)},
        {"executor_threads", std::to_string(facts.executor_threads)}};
}

}  // namespace rodaq::canonical
