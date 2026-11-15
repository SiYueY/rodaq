#pragma once

#include <memory>
#include <mutex>

#include <rodaq/acquisition/types.hpp>

#include "admission_gate.hpp"
#include "record_budget.hpp"
#include "record_queue.hpp"
#include "segment_capacity.hpp"
#include "sequence_allocator.hpp"

namespace rodaq::pipeline {

// Controller owns this fixed Stream entry through actual callback completion.
// Descriptor Cut, sequence refill completion, Segment cursor and Admission
// finalization use this same short mutex. Never hold it across Native or I/O.
struct StreamAdmission {
    std::mutex mutex;
    const StreamId stream_id;
    const std::uint64_t max_record_bytes;
    DescriptorRef active_descriptor;
    std::shared_ptr<SegmentCapacity> segment;
    SequenceAllocator sequences;

    StreamAdmission(StreamId identity, std::uint64_t max_bytes) noexcept
    : stream_id(identity), max_record_bytes(max_bytes) {}
};

// All references are instance-scoped backing retained until actual callbacks
// return. Control generation is immutable for this actual instance.
struct AdmissionDomain {
    TaskId task_id;
    PipelineId pipeline_id;
    InstanceId instance_id;
    ControlGeneration generation;
    AdmissionGate& gate;
    RecordBudget& records;
    PayloadPool& payloads;
    RecordQueue& queue;
};

// Core captures a fresh receive sample/Epoch before calling this function.
// Borrowed plugin input is retained only through this COPY call, never queued.
Result<acquisition::SubmitResult> submit_record(
    AdmissionDomain& domain, StreamAdmission& stream, const acquisition::RecordAttempt& attempt,
    const time::ClockSnapshot& received);

}  // namespace rodaq::pipeline
