#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>

#include <rodaq/acquisition/types.hpp>
#include <rodaq/canonical/validation.hpp>
#include <rodaq/plugin/abi.hpp>

namespace rodaq::acquisition::abi {

inline bool valid_prepare(const PrepareConfig* config) noexcept {
    if (!plugin::abi::readable(config) || config->stream_count == 0 ||
        config->max_callback_concurrency == 0 || config->streams == nullptr ||
        reinterpret_cast<std::uintptr_t>(config->streams) % alignof(StreamBinding) != 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < config->stream_count; ++i) {
        const auto& stream = config->streams[i];
        if (stream.header.struct_size != sizeof(StreamBinding) || stream.stream_id == 0 ||
            stream.max_record_bytes == 0 || !plugin::abi::valid_string(stream.stream_key) ||
            stream.stream_key.size == 0) {
            return false;
        }
    }
    return true;
}

inline bool valid_record(const RecordAttempt* record) noexcept {
    if (!plugin::abi::readable(record) || record->stream_id == 0 ||
        record->descriptor_token.value == 0 || record->descriptor_token.generation == 0 ||
        record->payload.header.struct_size != sizeof(rodaq_payload_t) ||
        (record->payload.logical_size != 0 && record->payload.data == nullptr) ||
        record->timestamp_count > RODAQ_RECORD_TIMESTAMP_CAPACITY ||
        (record->delivery_flags & ~RODAQ_RECORD_DELIVERY_RETAINED) != 0 ||
        !canonical::valid_source_facts(
            record->source_id, record->source_session_id, record->source_sequence,
            record->sequence_scope, record->has_source_id, record->has_source_session_id,
            record->has_source_sequence) ||
        !std::all_of(std::begin(record->reserved), std::end(record->reserved), [](auto byte) {
            return byte == 0;
        })) {
        return false;
    }
    for (std::uint32_t i = 0; i < RODAQ_RECORD_TIMESTAMP_CAPACITY; ++i) {
        if (!canonical::valid_timestamp(record->timestamps[i], i < record->timestamp_count)) {
            return false;
        }
    }
    return true;
}

inline const rodaq_ros2_subscription_facts_t* subscription_facts(
    const DescriptorProposal& proposal) noexcept {
    return plugin::abi::readable(&proposal) ? proposal.ros2_subscription : nullptr;
}

inline bool valid_subscription_facts(const rodaq_ros2_subscription_facts_t* facts) noexcept {
    return plugin::abi::readable(facts) && canonical::valid_text(facts->ros_distribution, 64) &&
           canonical::valid_text(facts->rmw_implementation, 128) &&
           canonical::valid_qos(facts->installed_requested_qos) &&
           facts->subscription_generation != 0 && facts->executor_kind == 1 &&
           facts->executor_threads >= 1 && facts->executor_threads <= 8;
}

inline bool valid_descriptor(const DescriptorProposal* proposal) noexcept {
    if (!plugin::abi::readable(
            proposal, offsetof(DescriptorProposal, reserved1) + sizeof(proposal->reserved1))) {
        return false;
    }
    const auto* subscription = subscription_facts(*proposal);
    return (subscription == nullptr || (proposal->data_profile == RODAQ_DATA_PROFILE_ROS2 &&
                                        valid_subscription_facts(subscription))) &&
           proposal->stream_id != 0 && proposal->reserved0 == 0 && proposal->reserved1 == 0 &&
           canonical::valid_descriptor_fields(*proposal);
}

inline bool valid_source_event(const SourceEvent* event) noexcept {
    if (!plugin::abi::readable(event) || event->stream_id == 0 ||
        event->kind < RODAQ_SOURCE_EVENT_DISCOVERED ||
        event->kind > RODAQ_SOURCE_EVENT_SEQUENCE_RESET ||
        !canonical::valid_source_facts(
            event->source_id, event->source_session_id, event->source_sequence,
            event->sequence_scope, event->has_source_id, event->has_source_session_id,
            event->has_source_sequence) ||
        !std::all_of(std::begin(event->reserved), std::end(event->reserved), [](auto byte) {
            return byte == 0;
        })) {
        return false;
    }
    switch (event->kind) {
        case RODAQ_SOURCE_EVENT_DISCOVERED:
        case RODAQ_SOURCE_EVENT_LOST:
            return event->has_source_id != 0;
        case RODAQ_SOURCE_EVENT_SESSION_STARTED:
        case RODAQ_SOURCE_EVENT_SESSION_ENDED:
            return event->has_source_session_id != 0;
        case RODAQ_SOURCE_EVENT_SEQUENCE_GAP:
        case RODAQ_SOURCE_EVENT_SEQUENCE_RESET:
            return event->has_source_sequence != 0 &&
                   event->sequence_scope != RODAQ_SEQUENCE_SCOPE_OPAQUE;
        default:
            return false;
    }
}

inline bool valid_acquisition_event(const AcquisitionEvent* event) noexcept {
    if (!plugin::abi::readable(event) || event->has_stream_id > 1 || event->has_source_id > 1 ||
        (event->has_stream_id ? event->stream_id == 0 : event->stream_id != 0) ||
        !canonical::valid_source_id(event->source_id, event->has_source_id != 0) ||
        !plugin::abi::known_status(event->status) ||
        !std::all_of(std::begin(event->reserved), std::end(event->reserved), [](auto byte) {
            return byte == 0;
        })) {
        return false;
    }
    const bool failure = event->kind == RODAQ_ACQUISITION_EVENT_RECOVERABLE_FAILURE ||
                         event->kind == RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE;
    if (failure) {
        return event->status != RODAQ_STATUS_OK && event->count == 0;
    }
    if (event->status != RODAQ_STATUS_OK) {
        return false;
    }
    switch (event->kind) {
        case RODAQ_ACQUISITION_EVENT_TRANSPORT_READY:
        case RODAQ_ACQUISITION_EVENT_TRANSPORT_LOST:
            return event->has_source_id == 0 && event->count == 0;
        case RODAQ_ACQUISITION_EVENT_STREAM_READY:
        case RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE:
            return event->has_stream_id != 0 && event->has_source_id == 0 && event->count == 0;
        case RODAQ_ACQUISITION_EVENT_DROPPED:
        case RODAQ_ACQUISITION_EVENT_OVERFLOW:
        case RODAQ_ACQUISITION_EVENT_INVALID_PAYLOAD:
        case RODAQ_ACQUISITION_EVENT_SOURCE_TIMESTAMP_DISCONTINUITY:
            return event->has_stream_id != 0 && event->count != 0;
        default:
            return false;
    }
}

inline bool valid_submission(const SubmitResult& result) noexcept {
    return result.header.struct_size >= sizeof(SubmitResult) && result.reserved0 == 0 &&
           result.disposition >= RODAQ_SUBMIT_ACCEPTED &&
           result.disposition <= RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
}

}  // namespace rodaq::acquisition::abi
