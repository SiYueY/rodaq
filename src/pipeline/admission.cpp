#include "admission.hpp"

#include <cassert>
#include <limits>
#include <utility>

#include <rodaq/acquisition/validation.hpp>

namespace rodaq::pipeline {

Result<acquisition::RecordDisposition> submit_record(
    AdmissionDomain& domain, StreamAdmission& stream, const acquisition::RecordAttempt& attempt,
    const time::ClockSample& received) {
    if (!acquisition::abi::valid_record(&attempt) || attempt.stream_id != stream.stream_id) {
        return RODAQ_SUBMIT_INVALID_RECORD;
    }
    if (attempt.payload.size > stream.max_record_bytes) {
        return RODAQ_SUBMIT_RECORD_TOO_LARGE;
    }
    if (domain.gate.admission_fenced()) {
        return RODAQ_SUBMIT_GATE_CLOSED;
    }
    if (received.monotonic_ns < domain.opened_monotonic_ns) {
        return RODAQ_SUBMIT_INVALID_RECORD;
    }
    const auto elapsed = received.monotonic_ns - domain.opened_monotonic_ns;
    if (elapsed > std::numeric_limits<std::uint64_t>::max() - domain.recording_base_ns) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto recording_ns = domain.recording_base_ns + elapsed;
    auto projected = time::project_recording_time(domain.playback_anchor, received.monotonic_ns);
    if (!projected.ok()) {
        return RODAQ_SUBMIT_INVALID_RECORD;
    }

    DescriptorRef descriptor;
    std::shared_ptr<RecordingSegment> segment;
    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        if (!stream.active_descriptor || !stream.segment) {
            return RODAQ_SUBMIT_DESCRIPTOR_NOT_READY;
        }
        if (stream.active_descriptor->descriptor_id != attempt.descriptor_id) {
            return RODAQ_SUBMIT_STALE_DESCRIPTOR;
        }
        descriptor = stream.active_descriptor;
        segment = stream.segment;
    }

    auto charge = domain.records.reserve(attempt.payload.size);
    if (!charge.ok()) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    auto queue_slot = domain.queue.reserve();
    if (!queue_slot.ok()) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    auto candidate = segment->reserve_record(attempt.payload.size, received.monotonic_ns);
    if (!candidate.ok()) {
        if (segment->capacity_snapshot().admission_failure != StatusCode::Ok) {
            (void)domain.gate.establish_control_fence(domain.instance_id);
        }
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    auto metadata = segment->metadata.reserve_record(
        stream.stream_id, descriptor->descriptor_id, received, recording_ns);
    if (!metadata.ok()) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    auto payload = domain.payloads.reserve(attempt.payload.size);
    if (!payload.ok()) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    auto copied = payload.value().copy_from(attempt.payload.data, attempt.payload.size);
    if (!copied.ok()) {
        return RODAQ_SUBMIT_INVALID_RECORD;
    }

    Record record;
    record.charge = std::move(charge.value());
    record.payload = std::move(payload.value());
    record.descriptor = descriptor;
    record.task_id = domain.task_id;
    record.pipeline_id = domain.pipeline_id;
    record.instance_id = domain.instance_id;
    record.segment_id = segment->metadata.segment_id();
    record.stream_id = stream.stream_id;
    record.received = received;
    record.recording_time_ns = projected.value();
    record.publish_time_ns = attempt.source_publish_time;
    record.has_publish_time = attempt.has_source_publish_time;

    auto gate = domain.gate.enter_commit(domain.instance_id, domain.generation);
    if (!gate.ok()) {
        return RODAQ_SUBMIT_GATE_CLOSED;
    }
    std::lock_guard<std::mutex> lock(stream.mutex);
    if (stream.active_descriptor != descriptor) {
        return RODAQ_SUBMIT_STALE_DESCRIPTOR;
    }
    if (stream.segment != segment) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    if (stream.next_sequence == 0) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    auto transferred = candidate.value().transfer();
    if (!transferred.ok()) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    const auto sequence = stream.next_sequence;
    stream.next_sequence = sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : sequence + 1;
    record.acceptance_sequence = sequence;
    record.settlement = metadata.value().accept(sequence);
    domain.queue.publish(std::move(queue_slot.value()), std::move(record));
    const auto counted = gate.value().mark_committed();
    assert(counted);
    (void)counted;
    return RODAQ_SUBMIT_ACCEPTED;
}

}  // namespace rodaq::pipeline
