#include "runtime_state.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include <rclcpp/version.h>
#include <rmw/rmw.h>

#include <rodaq/canonical/validation.hpp>

namespace rodaq::ros2_acquisition {
namespace {

rodaq_string_t view(const std::string& text) {
    return {text.data(), static_cast<std::uint32_t>(text.size())};
}

rodaq_ros2_qos_profile_t project(const rmw_qos_profile_t& native) {
    rodaq_ros2_qos_profile_t qos{};
    qos.history = native.history <= 3 ? static_cast<std::uint32_t>(native.history)
                                      : static_cast<std::uint32_t>(RODAQ_ROS2_QOS_HISTORY_UNKNOWN);
    qos.reliability = native.reliability <= 3
                          ? static_cast<std::uint32_t>(native.reliability)
                          : static_cast<std::uint32_t>(RODAQ_ROS2_QOS_RELIABILITY_UNKNOWN);
    qos.durability = native.durability <= 3
                         ? static_cast<std::uint32_t>(native.durability)
                         : static_cast<std::uint32_t>(RODAQ_ROS2_QOS_DURABILITY_UNKNOWN);
    qos.liveliness = native.liveliness <= 4
                         ? static_cast<std::uint32_t>(native.liveliness)
                         : static_cast<std::uint32_t>(RODAQ_ROS2_QOS_LIVELINESS_UNKNOWN);
    qos.depth = native.depth;
    qos.deadline = {native.deadline.sec, native.deadline.nsec};
    qos.lifespan = {native.lifespan.sec, native.lifespan.nsec};
    qos.liveliness_lease_duration = {
        native.liveliness_lease_duration.sec, native.liveliness_lease_duration.nsec};
    qos.avoid_ros_namespace_conventions = native.avoid_ros_namespace_conventions;
    return qos;
}

}  // namespace

Result<Backend::State::Facts> Backend::State::inspect(const Stream& stream) {
    const auto endpoints = node->get_publishers_info_by_topic(stream.config.topic);
    if (endpoints.size() > 256) {
        return Status(StatusCode::ResourceExhausted, "ROS 2 endpoint bound exceeded");
    }
    Facts facts;
    facts.type = stream.config.type.value_or("");
    if (endpoints.empty() && facts.type.empty() && stream.snapshot) {
        facts.type = stream.snapshot->facts.type;
    }
    for (const auto& endpoint : endpoints) {
        if (!ros2::valid_type_name(endpoint.topic_type())) {
            return Status(StatusCode::InvalidArgument, "Invalid ROS 2 discovered type");
        }
        if (facts.type.empty()) {
            facts.type = endpoint.topic_type();
        } else if (facts.type != endpoint.topic_type()) {
            return Status(StatusCode::Conflict, "Conflicting ROS 2 topic types");
        }
        auto qos = project(endpoint.qos_profile().get_rmw_qos_profile());
        facts.offers.push_back(qos);
        const auto& effective_req =
            stream.snapshot ? stream.snapshot->installed : stream.config.requested;
        if (ros2::qos_compatible(effective_req, {qos})) {
            ++facts.compatible_publishers;
        }
    }
    if (facts.type.empty()) {
        return Status(StatusCode::Unavailable, "ROS 2 type unavailable without a publisher");
    }
    auto normalized = ros2::normalize_qos(std::move(facts.offers));
    if (!normalized.ok()) {
        return normalized.status();
    }
    facts.offers = std::move(normalized.value());
    return facts;
}

Result<void> Backend::State::bind_stream(Stream& stream, Facts facts, plugin::Deadline deadline) {
    try {
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->facts = std::move(facts);
        snapshot->installed =
            stream.config.adapt
                ? ros2::adapt_qos(snapshot->facts.offers, stream.config.requested.depth)
                : stream.config.requested;

        auto schema = ros2::resolve_schema(snapshot->facts.type);
        if (!schema.ok()) {
            return schema.status();
        }
        auto library = resident_typesupport(snapshot->facts.type, deadline);
        if (!library.ok()) {
            return library.status();
        }
        snapshot->schema = std::move(schema.value());

        // Create subscription before proposing Descriptor so actual installed
        // requested QoS is observed and recorded in the bind-once Descriptor.
        stream.subscription = subscribe(stream, *snapshot);
        snapshot->actual_request =
            project(stream.subscription->get_actual_qos().get_rmw_qos_profile());

        const auto* rmw = rmw_get_implementation_identifier();
        if (rmw == nullptr) {
            return Status(StatusCode::Unavailable, "ROS 2 runtime identity unavailable");
        }
        const auto actual_threads = executor->get_number_of_threads();
        if (actual_threads == 0 || actual_threads > 8) {
            return Status(StatusCode::InvalidState, "ROS 2 executor outside qualified bounds");
        }
#if RCLCPP_VERSION_MAJOR == 16
        constexpr rodaq_string_t distribution{"humble", 6};
#elif RCLCPP_VERSION_MAJOR == 28
        constexpr rodaq_string_t distribution{"jazzy", 5};
#else
#error Unsupported ROS 2 distribution for the qualified backend
#endif
        const rodaq_ros2_subscription_facts_t subscription_facts{
            {sizeof(rodaq_ros2_subscription_facts_t)},
            distribution,
            {rmw, static_cast<std::uint32_t>(std::strlen(rmw))},
            snapshot->actual_request,
            1,
            1,
            static_cast<std::uint32_t>(actual_threads)};

        acquisition::Descriptor descriptor{};
        descriptor.header.struct_size = sizeof(descriptor);
        descriptor.stream_id = stream.id;
        descriptor.data_profile = RODAQ_DATA_PROFILE_ROS2;
        descriptor.channel_name = view(stream.config.topic);
        descriptor.message_encoding = {"cdr", 3};
        descriptor.schema_name = view(snapshot->schema.name);
        descriptor.schema_encoding = view(snapshot->schema.encoding);
        descriptor.schema = {
            reinterpret_cast<const std::uint8_t*>(snapshot->schema.text.data()),
            static_cast<std::uint32_t>(snapshot->schema.text.size())};
        descriptor.ros2_offered_qos_profiles =
            snapshot->facts.offers.empty() ? nullptr : snapshot->facts.offers.data();
        descriptor.ros2_offered_qos_profile_count =
            static_cast<std::uint32_t>(snapshot->facts.offers.size());
        descriptor.ros2_subscription = &subscription_facts;

        auto descriptor_id = callbacks.prepare_descriptor(stream.id, descriptor);
        if (!descriptor_id.ok()) {
            stream.subscription.reset();
            return descriptor_id.status();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                stream.subscription.reset();
                return Status(StatusCode::Conflict, "ROS 2 stopped during bind_stream");
            }
            snapshot->descriptor_id = descriptor_id.value();
            stream.snapshot = std::move(snapshot);
            stream.ready_reported = true;
        }
        report_event(RODAQ_ACQUISITION_EVENT_STREAM_READY, stream.id);
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "ROS 2 subscription allocation failed");
    } catch (const std::exception&) {
        return Status(StatusCode::Unavailable, "ROS 2 interpretation or subscription unavailable");
    } catch (...) {
        return Status(StatusCode::Internal, "Unknown ROS 2 subscription exception");
    }
}

rclcpp::GenericSubscription::SharedPtr Backend::State::subscribe(
    Stream& stream, const Snapshot& snapshot) {
    rclcpp::QoS qos(rclcpp::KeepLast(snapshot.installed.depth));
    qos.reliability(
        snapshot.installed.reliability == RODAQ_ROS2_QOS_RELIABILITY_RELIABLE
            ? RMW_QOS_POLICY_RELIABILITY_RELIABLE
            : RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT);
    qos.durability(
        snapshot.installed.durability == RODAQ_ROS2_QOS_DURABILITY_TRANSIENT_LOCAL
            ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL
            : RMW_QOS_POLICY_DURABILITY_VOLATILE);
    rclcpp::SubscriptionOptions options;
    options.callback_group = stream.group;
    options.event_callbacks.message_lost_callback = [this,
                                                     &stream](rclcpp::QOSMessageLostInfo& info) {
        if (info.total_count_change > 0) {
            std::lock_guard<std::mutex> lock(mutex);
            const auto increment = static_cast<std::uint64_t>(info.total_count_change);
            stream.lost +=
                std::min(increment, std::numeric_limits<std::uint64_t>::max() - stream.lost);
        }
    };
#if RCLCPP_VERSION_MAJOR >= 28
    auto callback = [this, &stream](
                        std::shared_ptr<rclcpp::SerializedMessage> message,
                        const rclcpp::MessageInfo& info) {
        dispatch(stream, message, &info.get_rmw_message_info());
    };
#else
    auto callback = [this, &stream](std::shared_ptr<rclcpp::SerializedMessage> message) {
        dispatch(stream, message, nullptr);
    };
#endif
    return node->create_generic_subscription(
        stream.config.topic, snapshot.facts.type, qos, callback, options);
}

void Backend::State::dispatch(
    Stream& stream, const std::shared_ptr<rclcpp::SerializedMessage>& message,
    const rmw_message_info_t* metadata) noexcept {
    auto permit = data_permit(stream);
    if (!permit) {
        return;
    }
    try {
        if (!message) {
            report_event(RODAQ_ACQUISITION_EVENT_INVALID_PAYLOAD, stream.id, StatusCode::Ok, 1);
            return;
        }
        const auto& raw = message->get_rcl_serialized_message();
        acquisition::RecordAttempt attempt{};
        attempt.header.struct_size = sizeof(attempt);
        attempt.stream_id = stream.id;
        attempt.descriptor_id = permit.snapshot->descriptor_id;
        attempt.payload = {raw.buffer, static_cast<std::uint32_t>(raw.buffer_length)};
#if RCLCPP_VERSION_MAJOR >= 28
        if (metadata != nullptr && metadata->source_timestamp > 0) {
            attempt.source_publish_time = static_cast<std::uint64_t>(metadata->source_timestamp);
            attempt.has_source_publish_time = 1;
        }
#else
        (void)metadata;
#endif
        const auto disposition = callbacks.submit_record(attempt);
        if (disposition == RODAQ_SUBMIT_INVALID) {
            report_event(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, stream.id, StatusCode::Internal);
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            changed.notify_all();
        }
    } catch (...) {
        report_event(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, stream.id, StatusCode::Internal);
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        changed.notify_all();
    }
}

void Backend::State::flush_events(Stream& stream) noexcept {
    for (const auto kind : {RODAQ_ACQUISITION_EVENT_OVERFLOW, RODAQ_ACQUISITION_EVENT_DROPPED}) {
        std::uint64_t count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto& counter =
                kind == RODAQ_ACQUISITION_EVENT_OVERFLOW ? stream.overflow : stream.lost;
            count = std::exchange(counter, 0);
        }
        if (count != 0) {
            report_event(kind, stream.id, StatusCode::Ok, count);
        }
    }
}

void Backend::State::observe() noexcept {
    try {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (changed.wait_for(lock, config->graph_poll_interval, [&] { return stopping; })) {
                    break;
                }
            }
            for (auto& stream : streams) {
                const auto deadline = plugin::Deadline::after(std::chrono::seconds{2}).value();
                auto permit = control_permit(deadline);
                if (!permit) {
                    continue;
                }
                flush_events(*stream);
                auto facts = inspect(*stream);
                if (!facts.ok()) {
                    // Incompatible topic type or endpoint overflow after Gate Open:
                    // fail-closed; do not hot-rebuild subscription or cut Descriptor.
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        stream->gate = false;
                    }
                    report_event(RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE, stream->id);
                    report_event(
                        RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, stream->id,
                        facts.status().code());
                    continue;
                }
                const auto& bound = *stream->snapshot;
                const bool type_changed = facts.value().type != bound.facts.type;
                const bool qos_incompatible =
                    !facts.value().offers.empty() &&
                    !ros2::qos_compatible(bound.installed, facts.value().offers);
                if (type_changed || (stream->config.adapt && qos_incompatible)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        stream->gate = false;
                    }
                    report_event(RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE, stream->id);
                    report_event(
                        RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, stream->id, StatusCode::Conflict);
                    continue;
                }
                // Compatible publisher join/leave does not rebuild subscription.
                if (!stream->ready_reported) {
                    stream->ready_reported = true;
                    report_event(RODAQ_ACQUISITION_EVENT_STREAM_READY, stream->id);
                }
            }
        }
    } catch (...) {
        fail(StatusCode::Internal);
    }
    std::lock_guard<std::mutex> lock(mutex);
    graph_done = true;
    changed.notify_all();
}

}  // namespace rodaq::ros2_acquisition
