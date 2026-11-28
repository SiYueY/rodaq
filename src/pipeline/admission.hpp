#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

#include <rodaq/acquisition/types.hpp>

#include "../time/clock.hpp"
#include "admission_gate.hpp"
#include "record_budget.hpp"
#include "record_queue.hpp"
#include "recording_segment.hpp"

namespace rodaq::pipeline {

// Controller owns this fixed Stream entry through actual callback completion.
// Bind-once Descriptor is frozen during Prepare; AcceptanceSequence is
// instance-local starting at 1. Never hold mutex across Native or I/O.
struct StreamAdmission {
    std::mutex mutex;
    std::condition_variable segment_changed;
    const StreamId stream_id;
    const std::uint64_t max_record_bytes;
    DescriptorRef active_descriptor;
    std::shared_ptr<RecordingSegment> segment;
    std::uint64_t next_sequence{1};
    bool segment_transition{false};

    StreamAdmission(StreamId identity, std::uint64_t max_bytes) noexcept
    : stream_id(identity), max_record_bytes(max_bytes) {}
};

// All references are instance-scoped backing retained until actual callbacks
// return. Control generation is immutable for this actual instance.
struct AdmissionDomain {
    const TaskId task_id;
    const PipelineId pipeline_id;
    const InstanceId instance_id;
    const ControlGeneration generation;
    AdmissionGate& gate;
    RecordBudget& records;
    PayloadPool& payloads;
    RecordQueue& queue;
    const time::PlaybackAnchor playback_anchor;
    const std::uint64_t recording_base_ns{0};
    const std::uint64_t opened_monotonic_ns{0};
};

// Core captures a fresh receive clock sample before calling this function.
// Borrowed plugin input is retained only through this COPY call, never queued.
Result<acquisition::RecordDisposition> submit_record(
    AdmissionDomain& domain, StreamAdmission& stream, const acquisition::RecordAttempt& attempt,
    const time::ClockSample& received);

}  // namespace rodaq::pipeline
