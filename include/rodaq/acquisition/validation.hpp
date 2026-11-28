#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

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

inline bool valid_callbacks(const CallbacksTable* callbacks) noexcept {
    return plugin::abi::readable(
               callbacks,
               offsetof(CallbacksTable, report_event) + sizeof(callbacks->report_event)) &&
           callbacks->prepare_descriptor != nullptr && callbacks->submit_record != nullptr &&
           callbacks->report_event != nullptr;
}

inline bool valid_record(const RecordAttempt* record) noexcept {
    return plugin::abi::readable(record) && record->stream_id != 0 && record->descriptor_id != 0 &&
           (record->payload.size == 0 || record->payload.data != nullptr) &&
           record->has_source_publish_time <= 1 &&
           (record->has_source_publish_time != 0 || record->source_publish_time == 0) &&
           std::all_of(std::begin(record->reserved), std::end(record->reserved), [](auto byte) {
               return byte == 0;
           });
}

inline bool valid_descriptor(const Descriptor* descriptor) noexcept {
    return plugin::abi::readable(descriptor) && descriptor->stream_id != 0 &&
           canonical::valid_descriptor_fields(*descriptor);
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

inline bool valid_disposition(RecordDisposition disposition) noexcept {
    return disposition >= RODAQ_SUBMIT_ACCEPTED && disposition <= RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
}

}  // namespace rodaq::acquisition::abi
