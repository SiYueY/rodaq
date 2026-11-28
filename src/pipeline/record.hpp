#pragma once

#include <cstdint>

#include <rodaq/acquisition/types.hpp>
#include <rodaq/storage/types.hpp>
#include <rodaq/task/types.hpp>

#include "../time/clock.hpp"
#include "descriptor.hpp"
#include "instance_id.hpp"
#include "payload_pool.hpp"
#include "record_budget.hpp"
#include "segment_metadata.hpp"

namespace rodaq::pipeline {

// Constructed outside Gate, then sequence/descriptor binding is finalized in
// the Stream commit domain before the complete value is published to the queue.
struct Record {
    // Declared first so the charge is released after all payload/descriptor use.
    RecordCharge charge;
    RecordSettlement settlement;
    Payload payload;
    DescriptorRef descriptor;
    TaskId task_id;
    PipelineId pipeline_id{0};
    InstanceId instance_id;
    rodaq_segment_id_t segment_id{0};
    StreamId stream_id{0};
    std::uint64_t acceptance_sequence{0};
    time::ClockSample received;
    // Projected epoch-anchored monotonic recording timestamp written to Storage.
    std::uint64_t recording_time_ns{0};
    std::uint64_t publish_time_ns{0};
    std::uint8_t has_publish_time{0};

    Record() noexcept = default;
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;
    Record(Record&&) noexcept = default;
    Record& operator=(Record&&) noexcept = default;

    storage::Record view() const noexcept;
};

}  // namespace rodaq::pipeline
