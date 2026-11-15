#pragma once

#include <array>
#include <cstdint>

#include <rodaq/acquisition/types.hpp>
#include <rodaq/task/types.hpp>

#include "descriptor.hpp"
#include "instance_id.hpp"
#include "payload_pool.hpp"
#include "record_budget.hpp"
#include "../time/clock.hpp"

namespace rodaq::pipeline {

// Constructed outside Gate, then sequence/descriptor binding is finalized in
// the Stream commit domain before the complete value is published to the queue.
struct Record {
    // Declared first so the charge is released after all payload/descriptor use.
    RecordCharge charge;
    Payload payload;
    DescriptorRef descriptor;
    TaskId task_id;
    PipelineId pipeline_id{0};
    InstanceId instance_id;
    // Resolves only within the owning instance's retained Segment reservations.
    // Writer cannot reassign this accepted Record to an unreserved Segment.
    rodaq_segment_id_t segment_id{0};
    StreamId stream_id{0};
    std::uint64_t acceptance_sequence{0};
    time::ClockSnapshot received;
    rodaq_source_id_t source_id{};
    rodaq_source_session_id_t source_session_id{0};
    std::uint64_t source_sequence{0};
    std::array<rodaq_record_timestamp_t, RODAQ_RECORD_TIMESTAMP_CAPACITY> timestamps{};
    std::uint32_t timestamp_count{0};
    rodaq_sequence_scope_t sequence_scope{RODAQ_SEQUENCE_SCOPE_INVALID};
    rodaq_record_delivery_flags_t delivery_flags{RODAQ_RECORD_DELIVERY_NONE};
    std::uint8_t has_source_id{0};
    std::uint8_t has_source_session_id{0};
    std::uint8_t has_source_sequence{0};

    Record() noexcept = default;
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;
    Record(Record&&) noexcept = default;
    Record& operator=(Record&&) noexcept = default;

    storage::Record view() const noexcept;
};

}  // namespace rodaq::pipeline
