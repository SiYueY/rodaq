#pragma once

#include <memory>
#include <vector>

#include "segment_capacity.hpp"
#include "segment_metadata.hpp"
#include "instance_id.hpp"

namespace rodaq::pipeline {

// One retained aggregate binds the physical and metadata owners to the same
// immutable Segment identity. Instance/actual Writer/Commit frames retain it;
// accepted Records resolve their numeric Segment ID through that instance.
class RecordingSegment {
public:
    static Result<std::shared_ptr<RecordingSegment>> prepare(
        artifact::ManifestIdentity identity, std::vector<StreamId> streams,
        const storage::PrepareConfig& envelope, const storage::CapacityModel& model,
        artifact::ManifestLimits manifest_limits, artifact::VolumeCapacity& volume,
        artifact::LedgerRegistration registration, std::uint64_t now_monotonic_ns);

    // Under the Gate cut, or after close_and_capture. This does not wait for
    // Native or release any accepted work/registration capacity.
    void close_admission();

    SegmentCapacity capacity;
    SegmentMetadata metadata;
    const TaskId task_id;
    const PipelineId pipeline_id;
    const InstanceId instance_id;
    const artifact::VolumeId volume_id;

private:
    RecordingSegment(
        SegmentCapacity physical, SegmentMetadata facts, InstanceId instance,
        artifact::VolumeId volume, TaskId task, PipelineId pipeline) noexcept;
};

}  // namespace rodaq::pipeline
