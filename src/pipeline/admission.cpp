#include "admission.hpp"

#include <cassert>
#include <utility>

#include <rodaq/acquisition/validation.hpp>

namespace rodaq::pipeline {
namespace {

acquisition::SubmitResult disposition(rodaq_submit_disposition_t value) noexcept {
    return {{sizeof(acquisition::SubmitResult)}, value, 0};
}

bool token_matches(acquisition::DescriptorToken left, acquisition::DescriptorToken right) noexcept {
    return left.value == right.value && left.generation == right.generation;
}

}  // namespace

Result<acquisition::SubmitResult> submit_record(
    AdmissionDomain& domain, StreamAdmission& stream, const acquisition::RecordAttempt& attempt,
    const time::ClockSnapshot& received) {
    if (!acquisition::abi::valid_record(&attempt) || attempt.stream_id != stream.stream_id ||
        received.boot_id == 0 || received.epoch_id == 0) {
        return disposition(RODAQ_SUBMIT_INVALID_RECORD);
    }
    if (attempt.payload.logical_size > stream.max_record_bytes) {
        return disposition(RODAQ_SUBMIT_RECORD_TOO_LARGE);
    }
    DescriptorRef descriptor;
    std::shared_ptr<SegmentCapacity> segment;
    {
        std::lock_guard<std::mutex> lock(stream.mutex);
        if (!stream.active_descriptor || !stream.segment) {
            return disposition(RODAQ_SUBMIT_DESCRIPTOR_NOT_READY);
        }
        if (!token_matches(stream.active_descriptor->token, attempt.descriptor_token)) {
            return disposition(RODAQ_SUBMIT_STALE_DESCRIPTOR);
        }
        descriptor = stream.active_descriptor;
        segment = stream.segment;
    }
    auto charge = domain.records.reserve(attempt.payload.logical_size);
    if (!charge.ok()) {
        return disposition(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    auto queue_slot = domain.queue.reserve();
    if (!queue_slot.ok()) {
        return disposition(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    auto candidate =
        segment->reserve_record(attempt.payload.logical_size, received.sample.monotonic_ns);
    if (!candidate.ok()) {
        return disposition(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    auto payload = domain.payloads.reserve(attempt.payload.logical_size);
    if (!payload.ok()) {
        return disposition(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    auto copied = payload.value().copy_from(attempt.payload.data, attempt.payload.logical_size);
    if (!copied.ok()) {
        return disposition(RODAQ_SUBMIT_INVALID_RECORD);
    }
    Record record;
    record.charge = std::move(charge.value());
    record.payload = std::move(payload.value());
    record.descriptor = descriptor;
    record.task_id = domain.task_id;
    record.pipeline_id = domain.pipeline_id;
    record.instance_id = domain.instance_id;
    record.segment_id = segment->snapshot().segment_id;
    record.stream_id = stream.stream_id;
    record.received = received;
    record.source_id = attempt.source_id;
    record.source_session_id = attempt.source_session_id;
    record.source_sequence = attempt.source_sequence;
    record.timestamps = {};
    for (std::uint32_t index = 0; index < attempt.timestamp_count; ++index) {
        record.timestamps[index] = attempt.timestamps[index];
    }
    record.timestamp_count = attempt.timestamp_count;
    record.sequence_scope = attempt.sequence_scope;
    record.delivery_flags = attempt.delivery_flags;
    record.has_source_id = attempt.has_source_id;
    record.has_source_session_id = attempt.has_source_session_id;
    record.has_source_sequence = attempt.has_source_sequence;

    auto gate = domain.gate.enter_commit(domain.instance_id, domain.generation);
    if (!gate.ok()) {
        return disposition(RODAQ_SUBMIT_GATE_CLOSED);
    }
    std::lock_guard<std::mutex> lock(stream.mutex);
    if (stream.active_descriptor != descriptor) {
        return disposition(RODAQ_SUBMIT_STALE_DESCRIPTOR);
    }
    if (stream.segment != segment) {
        return disposition(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    const auto sequence = stream.sequences.snapshot();
    if (sequence.active_remaining == 0 && sequence.prepared_remaining == 0) {
        return disposition(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    auto transferred = candidate.value().transfer();
    if (!transferred.ok()) {
        return disposition(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    // Under this Stream domain, availability cannot change after the check.
    auto allocated = stream.sequences.allocate();
    assert(allocated.ok());
    record.acceptance_sequence = allocated.value();
    domain.queue.publish(std::move(queue_slot.value()), std::move(record));
    const auto counted = gate.value().mark_committed();
    assert(counted);
    (void)counted;
    return disposition(RODAQ_SUBMIT_ACCEPTED);
}

}  // namespace rodaq::pipeline
