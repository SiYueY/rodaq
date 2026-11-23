#include "runtime_state.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include <rclcpp/version.h>
#include <rmw/rmw.h>

#include "../../../src/ros2_qos.hpp"

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

bool source_less(const rodaq_source_id_t& left, const rodaq_source_id_t& right) {
    return std::memcmp(left.data, right.data, RODAQ_SOURCE_ID_DATA_CAPACITY) < 0;
}

bool source_equal(const rodaq_source_id_t& left, const rodaq_source_id_t& right) {
    return left.size == right.size && !source_less(left, right) && !source_less(right, left);
}

template <typename Gid>
rodaq_source_id_t source_id(const Gid& gid) {
    static_assert(sizeof(gid) <= RODAQ_SOURCE_ID_DATA_CAPACITY);
    rodaq_source_id_t id{};
    if (std::any_of(std::begin(gid), std::end(gid), [](auto byte) { return byte != 0; })) {
        id.kind = RODAQ_SOURCE_ID_ROS2_GID;
        id.size = sizeof(gid);
        std::copy(std::begin(gid), std::end(gid), id.data);
    }
    return id;
}

}  // namespace

bool Backend::State::same_facts(const Facts& left, const Facts& right) {
    return left.type == right.type && left.offers.size() == right.offers.size() &&
           std::equal(
               left.offers.begin(), left.offers.end(), right.offers.begin(),
               [](const auto& a, const auto& b) {
                   return canonical::qos_key(a) == canonical::qos_key(b);
               }) &&
           left.sources.size() == right.sources.size() &&
           std::equal(
               left.sources.begin(), left.sources.end(), right.sources.begin(), source_equal);
}

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
        if (stream.config.adapt || ros2::qos_compatible(stream.config.requested, {qos})) {
            auto id = source_id(endpoint.endpoint_gid());
            if (id.size != 0) {
                facts.sources.push_back(id);
            }
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
    std::sort(facts.sources.begin(), facts.sources.end(), source_less);
    facts.sources.erase(
        std::unique(facts.sources.begin(), facts.sources.end(), source_equal), facts.sources.end());
    return facts;
}

Result<void> Backend::State::sources(Stream& stream, const Facts* previous, const Facts& current) {
    const auto report = [&](const rodaq_source_id_t& source, rodaq_source_event_kind_t kind) {
        acquisition::SourceEvent event{};
        event.header.struct_size = sizeof(event);
        event.stream_id = stream.id;
        event.source_id = source;
        event.has_source_id = 1;
        event.kind = kind;

        return service->report_source_event(event);
    };
    if (previous != nullptr) {
        for (const auto& source : previous->sources) {
            if (!std::binary_search(
                    current.sources.begin(), current.sources.end(), source, source_less)) {
                auto result = report(source, RODAQ_SOURCE_EVENT_LOST);
                if (!result.ok()) {
                    return result;
                }
            }
        }
    }
    for (const auto& source : current.sources) {
        if (previous == nullptr ||
            !std::binary_search(
                previous->sources.begin(), previous->sources.end(), source, source_less)) {
            auto result = report(source, RODAQ_SOURCE_EVENT_DISCOVERED);
            if (!result.ok()) {
                return result;
            }
        }
    }
    return {};
}

Result<void> Backend::State::install(Stream& stream, Facts facts, plugin::Deadline deadline) {
    try {
        auto next = std::make_shared<Snapshot>();
        auto old = stream.snapshot;
        const bool rebuild =
            !old || old->facts.type != facts.type ||
            (stream.config.adapt && !ros2::qos_compatible(old->installed, facts.offers));
        next->facts = std::move(facts);
        next->installed = !rebuild ? old->installed
                          : stream.config.adapt
                              ? ros2::adapt_qos(next->facts.offers, stream.config.requested.depth)
                              : stream.config.requested;
        next->generation = old ? old->generation + static_cast<unsigned>(rebuild) : 1;
        if (next->generation == 0) {
            return Status(StatusCode::ResourceExhausted, "ROS 2 subscription generation exhausted");
        }
        if (old && old->facts.type == next->facts.type) {
            next->schema = old->schema;
        } else {
            auto schema = ros2::resolve_schema(next->facts.type);
            if (!schema.ok()) {
                return schema.status();
            }
            // Validate loadability before offering an interpretation to Core.
            auto library = resident_typesupport(next->facts.type, deadline);
            if (!library.ok()) {
                return library.status();
            }
            next->schema = std::move(schema.value());
        }

        {
            std::unique_lock<std::mutex> lock(mutex);
            stream.gate = false;
            const auto drained = [&] { return stopping || stream.active == 0; };
            if (deadline.is_infinite()) {
                changed.wait(lock, drained);
            } else {
                auto remaining = deadline.remaining();
                if (!remaining.ok() || !changed.wait_for(lock, remaining.value(), drained)) {
                    return Status(
                        StatusCode::DeadlineExceeded, "ROS 2 stream callbacks remain active");
                }
            }
            if (stopping) {
                return Status(StatusCode::Conflict, "ROS 2 preparation stopped");
            }
        }
        if (old && rebuild) {
            auto unavailable = event(RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE, stream.id);
            if (!unavailable.ok()) {
                return unavailable;
            }
            stream.subscription.reset();
        }

        if (rebuild) {
            stream.subscription = subscribe(stream, *next);
            next->actual_request =
                project(stream.subscription->get_actual_qos().get_rmw_qos_profile());
        } else {
            next->actual_request = old->actual_request;
        }
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
        const rodaq_ros2_subscription_facts_t subscription{
            {sizeof(rodaq_ros2_subscription_facts_t)},
            distribution,
            {rmw, static_cast<std::uint32_t>(std::strlen(rmw))},
            next->actual_request,
            next->generation,
            1,
            static_cast<std::uint32_t>(actual_threads)};
        acquisition::DescriptorProposal proposal{};
        proposal.header.struct_size = sizeof(proposal);
        proposal.stream_id = stream.id;
        proposal.data_profile = RODAQ_DATA_PROFILE_ROS2;
        proposal.channel_name = view(stream.config.topic);
        proposal.message_encoding = {"cdr", 3};
        proposal.schema_name = view(next->schema.name);
        proposal.schema_encoding = view(next->schema.encoding);
        proposal.schema = {
            reinterpret_cast<const std::uint8_t*>(next->schema.text.data()),
            static_cast<std::uint32_t>(next->schema.text.size())};
        proposal.ros2_offered_qos_profiles =
            next->facts.offers.empty() ? nullptr : next->facts.offers.data();
        proposal.ros2_offered_qos_profile_count =
            static_cast<std::uint32_t>(next->facts.offers.size());
        proposal.ros2_subscription = &subscription;
        auto token = service->propose_descriptor(proposal, deadline);
        if (!token.ok()) {
            return token.status();
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                return Status(StatusCode::Conflict, "Late ROS 2 descriptor result after stop");
            }
        }
        next->token = token.value();
        auto source_result = sources(stream, old ? &old->facts : nullptr, next->facts);
        if (!source_result.ok()) {
            return source_result;
        }
        auto ready = event(RODAQ_ACQUISITION_EVENT_STREAM_READY, stream.id);
        if (!ready.ok()) {
            return ready;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            stream.snapshot = std::move(next);
            stream.gate = phase == Phase::Running && !stopping;
        }
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
    const auto generation = snapshot.generation;
#if RCLCPP_VERSION_MAJOR >= 28
    auto callback = [this, &stream, generation](
                        std::shared_ptr<rclcpp::SerializedMessage> message,
                        const rclcpp::MessageInfo& info) {
        dispatch(stream, generation, message, &info.get_rmw_message_info());
    };
#else
    auto callback = [this, &stream,
                     generation](std::shared_ptr<rclcpp::SerializedMessage> message) {
        dispatch(stream, generation, message, nullptr);
    };
#endif
    return node->create_generic_subscription(
        stream.config.topic, snapshot.facts.type, qos, callback, options);
}

void Backend::State::dispatch(
    Stream& stream, std::uint64_t generation,
    const std::shared_ptr<rclcpp::SerializedMessage>& message,
    const rmw_message_info_t* metadata) noexcept {
    auto permit = data_permit(stream, generation);
    if (!permit) {
        return;
    }
    try {
        if (!message) {
            (void)event(RODAQ_ACQUISITION_EVENT_INVALID_PAYLOAD, stream.id, StatusCode::Ok, 1);
            return;
        }
        const auto& raw = message->get_rcl_serialized_message();
        acquisition::RecordAttempt attempt{};
        attempt.header.struct_size = sizeof(attempt);
        attempt.stream_id = stream.id;
        attempt.descriptor_token = permit.snapshot->token;

        attempt.payload = {{sizeof(attempt.payload)}, raw.buffer, raw.buffer_length};
#if RCLCPP_VERSION_MAJOR >= 28
        if (metadata != nullptr) {
            attempt.source_id = source_id(metadata->publisher_gid.data);
            attempt.has_source_id = attempt.source_id.size != 0;
            if (attempt.has_source_id &&
                !std::binary_search(
                    permit.snapshot->facts.sources.begin(), permit.snapshot->facts.sources.end(),
                    attempt.source_id, source_less)) {
                // Public callback evidence can precede the graph snapshot.
                // Announce that actual GID before the record; no identity or
                // session is synthesized, and no per-message owner is retained.
                acquisition::SourceEvent discovered{};
                discovered.header.struct_size = sizeof(discovered);
                discovered.stream_id = stream.id;
                discovered.source_id = attempt.source_id;
                discovered.has_source_id = 1;
                discovered.kind = RODAQ_SOURCE_EVENT_DISCOVERED;
                (void)service->report_source_event(discovered);
            }
            if (attempt.has_source_id && metadata->publication_sequence_number !=
                                             RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED) {
                attempt.source_sequence = metadata->publication_sequence_number;
                attempt.has_source_sequence = 1;
                attempt.sequence_scope = RODAQ_SEQUENCE_SCOPE_SOURCE;
            }
            // No publication clock origin is assumed from the numeric timestamp.
        }
#else
        (void)metadata;
#endif
        auto result = service->submit_record(attempt);
        if (!result.ok()) {
            (void)event(
                RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, stream.id, result.status().code());
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            changed.notify_all();
        }
    } catch (...) {
        try {
            (void)event(RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE, stream.id, StatusCode::Internal);
        } catch (...) {
        }
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        changed.notify_all();
    }
}

Result<void> Backend::State::flush_events(Stream& stream) {
    for (const auto kind : {RODAQ_ACQUISITION_EVENT_OVERFLOW, RODAQ_ACQUISITION_EVENT_DROPPED}) {
        std::uint64_t count;
        {
            std::lock_guard<std::mutex> lock(mutex);
            count = kind == RODAQ_ACQUISITION_EVENT_OVERFLOW ? stream.overflow : stream.lost;
        }
        if (count != 0) {
            auto reported = event(kind, stream.id, StatusCode::Ok, count);
            if (!reported.ok()) {
                return reported;
            }
            std::lock_guard<std::mutex> lock(mutex);
            auto& counter =
                kind == RODAQ_ACQUISITION_EVENT_OVERFLOW ? stream.overflow : stream.lost;
            counter -= count;
        }
    }
    return {};
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
                auto flushed = flush_events(*stream);
                if (!flushed.ok()) {
                    continue;
                }
                auto facts = inspect(*stream);
                if (!facts.ok()) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        stream->gate = false;
                    }
                    (void)event(RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE, stream->id);
                    (void)event(
                        RODAQ_ACQUISITION_EVENT_RECOVERABLE_FAILURE, stream->id,
                        facts.status().code());
                    stream->candidate.reset();
                    continue;
                }
                bool gate;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    gate = stream->gate;
                }
                if (gate && same_facts(stream->snapshot->facts, facts.value())) {
                    stream->candidate.reset();
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                if (!stream->candidate || !same_facts(*stream->candidate, facts.value())) {
                    stream->candidate = std::move(facts.value());
                    stream->candidate_since = now;
                }
                if (now - stream->candidate_since < config->qos_change_debounce) {
                    continue;
                }
                auto result = install(*stream, std::move(*stream->candidate), deadline);
                stream->candidate.reset();
                if (!result.ok()) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        stream->gate = false;
                    }
                    (void)event(RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE, stream->id);
                    (void)event(
                        RODAQ_ACQUISITION_EVENT_RECOVERABLE_FAILURE, stream->id,
                        result.status().code());
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
