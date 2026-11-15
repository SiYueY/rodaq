#include "recording_segment.hpp"

#include <new>
#include <utility>

#include <rodaq/storage/validation.hpp>

namespace rodaq::pipeline {

RecordingSegment::RecordingSegment(SegmentCapacity physical, SegmentMetadata facts) noexcept
: capacity(std::move(physical)), metadata(std::move(facts)) {}

Result<std::shared_ptr<RecordingSegment>> RecordingSegment::prepare(
    artifact::ManifestIdentity identity, std::vector<StreamId> streams,
    const storage::PrepareConfig& envelope, const storage::CapacityModel& model,
    artifact::ManifestLimits manifest_limits, artifact::VolumeCapacity& volume,
    artifact::LedgerRegistration registration, std::uint64_t now_monotonic_ns) {
    if (registration.segment_id() != identity.segment_id ||
        !storage::abi::valid_prepare(&envelope) || !storage::abi::valid_capacity(model) ||
        envelope.max_record_bytes > envelope.max_batch_bytes ||
        envelope.system_boot_id != identity.playback_anchor.boot_id ||
        envelope.playback_anchor_monotonic != identity.playback_anchor.monotonic_ns ||
        envelope.playback_anchor_realtime != identity.playback_anchor.realtime_ns) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto segment_id = identity.segment_id;
    auto metadata =
        SegmentMetadata::create(std::move(identity), std::move(streams), envelope, manifest_limits);
    if (!metadata.ok()) {
        return metadata.status();
    }
    auto capacity = SegmentCapacity::create(
        segment_id, envelope, model, manifest_limits.max_bytes, volume, std::move(registration),
        now_monotonic_ns);
    if (!capacity.ok()) {
        return capacity.status();
    }
    try {
        return std::shared_ptr<RecordingSegment>(
            new RecordingSegment(std::move(capacity.value()), std::move(metadata.value())));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

void RecordingSegment::close_admission() {
    capacity.close_admission();
    metadata.close_admission();
}

}  // namespace rodaq::pipeline
