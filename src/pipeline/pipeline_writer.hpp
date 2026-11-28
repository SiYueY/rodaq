#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <rodaq/storage/interface.hpp>

#include "../artifact/managed_volume.hpp"
#include "admission_gate.hpp"
#include "record_queue.hpp"
#include "recording_segment.hpp"

namespace rodaq::pipeline {

struct WriterStep {
    StatusCode failure{StatusCode::Ok};
    std::uint32_t written{0};
    std::uint32_t known_failed{0};
    std::uint32_t unknown{0};
    std::optional<rodaq_segment_id_t> next_segment;
};

// One mutable execution lane per Pipeline. Composition owns scheduling and
// retains this owner, its Gate and Segment through actual calls/late return.
class PipelineWriter {
public:
    static Result<std::unique_ptr<PipelineWriter>> prepare(
        storage::Handle storage, const storage::PrepareConfig& envelope,
        std::shared_ptr<artifact::ManagedVolume> volume, AdmissionGate& gate, InstanceId instance,
        std::uint32_t max_prepared_descriptors, plugin::Deadline deadline);
    PipelineWriter(const PipelineWriter&) = delete;
    PipelineWriter& operator=(const PipelineWriter&) = delete;

    Result<void> prepare_descriptor(
        std::shared_ptr<RecordingSegment> segment, DescriptorRef descriptor,
        plugin::Deadline deadline);
    Result<WriterStep> write_available(
        RecordQueue& queue, std::shared_ptr<RecordingSegment> segment, plugin::Deadline deadline);
    Result<storage::SegmentSealResult> seal(
        std::shared_ptr<RecordingSegment> segment, plugin::Deadline deadline);
    Result<void> quiesce(plugin::Deadline deadline);

private:
    PipelineWriter(
        storage::Handle storage, storage::PrepareConfig envelope,
        std::shared_ptr<artifact::ManagedVolume> volume, AdmissionGate& gate,
        InstanceId instance) noexcept;
    Result<void> open(std::shared_ptr<RecordingSegment> segment, plugin::Deadline deadline);
    Result<void> observe_usage(RecordingSegment& segment);
    void fail(StatusCode failure) noexcept;
    Result<void> settle(WriterStep& progress, std::uint32_t prefix, bool unknown);

    storage::Handle storage_;
    const storage::PrepareConfig envelope_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    AdmissionGate& gate_;
    const InstanceId instance_;
    std::vector<Record> batch_;
    std::vector<storage::Record> views_;
    std::shared_ptr<RecordingSegment> active_;
    std::vector<DescriptorRef> catalog_;
    std::uint32_t catalog_capacity_{0};
    StatusCode failure_{StatusCode::Ok};
    bool seal_attempted_{false};
    bool quiescent_{false};
};

}  // namespace rodaq::pipeline
