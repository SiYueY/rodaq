#include "recording_segment.hpp"

#include <limits>
#include <new>
#include <utility>

#include <rodaq/storage/validation.hpp>

namespace rodaq::pipeline {

RecordCandidate::RecordCandidate(
    std::shared_ptr<RecordingSegment> owner, std::uint64_t input_bytes) noexcept
: owner_(std::move(owner)), input_bytes_(input_bytes) {}

RecordCandidate::RecordCandidate(RecordCandidate&& other) noexcept
: owner_(std::move(other.owner_)), input_bytes_(std::exchange(other.input_bytes_, 0)) {}

RecordCandidate::~RecordCandidate() noexcept {
    if (owner_) {
        std::lock_guard<std::mutex> lock(owner_->mutex_);
        --owner_->capacity_.candidates;
    }
}

Result<void> RecordCandidate::transfer() {
    if (!owner_) {
        return Status(StatusCode::InvalidState, {});
    }
    auto owner = std::move(owner_);
    std::lock_guard<std::mutex> lock(owner->mutex_);
    --owner->capacity_.candidates;
    if (!owner->capacity_.writable || owner->capacity_.admission_failure != StatusCode::Ok ||
        owner->capacity_.records >= owner->envelope_.max_segment_records ||
        input_bytes_ > owner->envelope_.max_segment_input_bytes - owner->capacity_.input_bytes) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    ++owner->capacity_.records;
    owner->capacity_.input_bytes += input_bytes_;
    return {};
}

RecordingSegment::RecordingSegment(
    SegmentMetadata facts, storage::PrepareConfig envelope, std::uint64_t manifest_reserve,
    artifact::VolumeReservation reservation, std::uint64_t reserved_bytes, InstanceId instance,
    artifact::VolumeId volume, TaskId task, PipelineId pipeline) noexcept
: metadata(std::move(facts)),
  task_id(task),
  pipeline_id(pipeline),
  instance_id(instance),
  volume_id(volume),
  envelope_(envelope),
  manifest_reserve_(manifest_reserve),
  reservation_(std::move(reservation)) {
    capacity_.segment_id = metadata.segment_id();
    capacity_.reserved_bytes = reserved_bytes;
    capacity_.writable = true;
}

Result<std::shared_ptr<RecordingSegment>> RecordingSegment::prepare(
    artifact::ManifestIdentity identity, std::vector<StreamId> streams,
    const storage::PrepareConfig& envelope, artifact::ManifestLimits manifest_limits,
    artifact::VolumeCapacity& volume, std::uint64_t reservation_bytes,
    std::uint64_t now_monotonic_ns) {
    if (!storage::abi::valid_prepare(&envelope) ||
        envelope.max_record_bytes > envelope.max_batch_bytes || reservation_bytes == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto instance = parse_instance_id(identity.instance_id);
    if (!instance.ok()) {
        return instance.status();
    }
    const auto volume_id = identity.volume_id;
    const auto task_id = identity.task_id;
    const auto pipeline_id = identity.pipeline_id;
    auto metadata =
        SegmentMetadata::create(std::move(identity), std::move(streams), envelope, manifest_limits);
    if (!metadata.ok()) {
        return metadata.status();
    }
    auto reserved = volume.reserve(reservation_bytes, now_monotonic_ns);
    if (!reserved.ok()) {
        return reserved.status();
    }
    const std::uint64_t manifest_reserve =
        std::min<std::uint64_t>(manifest_limits.max_bytes, reservation_bytes / 4);
    try {
        return std::shared_ptr<RecordingSegment>(new RecordingSegment(
            std::move(metadata.value()), envelope, manifest_reserve, std::move(reserved.value()),
            reservation_bytes, instance.value(), volume_id, task_id, pipeline_id));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<RecordCandidate> RecordingSegment::reserve_record(
    std::uint64_t logical_bytes, std::uint64_t now_monotonic_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capacity_.writable || !reservation_) {
        return Status(StatusCode::InvalidState, {});
    }
    if (capacity_.admission_failure != StatusCode::Ok) {
        return Status(capacity_.admission_failure, {});
    }
    if (logical_bytes > envelope_.max_record_bytes ||
        capacity_.records >= envelope_.max_segment_records ||
        logical_bytes > envelope_.max_segment_input_bytes - capacity_.input_bytes ||
        capacity_.actual_bytes >= capacity_.reserved_bytes) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto space = reservation_->space_snapshot(now_monotonic_ns);
    if (space.protection != StatusCode::Ok) {
        capacity_.admission_failure = space.protection;
        return Status(space.protection, {});
    }
    if (space.condition == artifact::VolumeSpaceCondition::Stopped) {
        capacity_.admission_failure = StatusCode::ResourceExhausted;
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (space.condition == artifact::VolumeSpaceCondition::Unknown ||
        space.condition == artifact::VolumeSpaceCondition::Recovering) {
        capacity_.admission_failure = StatusCode::Unavailable;
        return Status(StatusCode::Unavailable, {});
    }
    ++capacity_.candidates;
    return RecordCandidate(shared_from_this(), logical_bytes);
}

Result<void> RecordingSegment::observe_usage(const storage::SegmentUsage& usage) {
    if (usage.header.struct_size != sizeof(usage) ||
        usage.file_count > envelope_.max_segment_file_count) {
        invalidate_usage();
        return Status(StatusCode::DataLoss, {});
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reservation_) {
        return Status(StatusCode::InvalidState, {});
    }
    if (usage.physical_bytes > capacity_.reserved_bytes) {
        reservation_->invalidate_usage();
        capacity_.admission_failure = StatusCode::DataLoss;
        return Status(StatusCode::DataLoss, {});
    }
    const auto remaining = capacity_.reserved_bytes - usage.physical_bytes;
    auto updated = reservation_->record_usage(usage.physical_bytes, remaining);
    if (!updated.ok()) {
        capacity_.admission_failure = updated.status().code();
        return updated.status();
    }
    capacity_.actual_bytes = usage.physical_bytes;
    if (usage.physical_bytes + manifest_reserve_ > capacity_.reserved_bytes) {
        capacity_.admission_failure = StatusCode::ResourceExhausted;
    }
    return {};
}

void RecordingSegment::mark_effects_possible() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reservation_) {
        reservation_->mark_effects_possible();
    }
}

void RecordingSegment::invalidate_usage() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reservation_) {
        reservation_->invalidate_usage();
    }
    capacity_.admission_failure = StatusCode::DataLoss;
}

void RecordingSegment::close_admission() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_.writable = false;
    }
    metadata.close_admission();
}

Result<artifact::CommitReservation> RecordingSegment::handoff_sealed(std::uint64_t storage_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_.writable || capacity_.candidates != 0 || !reservation_ || capacity_.sealed) {
        return Status(StatusCode::InvalidState, {});
    }
    if (storage_bytes > capacity_.reserved_bytes) {
        reservation_->invalidate_usage();
        return Status(StatusCode::DataLoss, {});
    }
    auto updated =
        reservation_->record_usage(storage_bytes, capacity_.reserved_bytes - storage_bytes);
    if (!updated.ok()) {
        return updated.status();
    }
    capacity_.actual_bytes = storage_bytes;
    capacity_.sealed = true;
    artifact::CommitReservation result{std::move(*reservation_), storage_bytes};
    reservation_.reset();
    return result;
}

Result<void> RecordingSegment::settle_aborted_after_cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!reservation_) {
        return Status(StatusCode::InvalidState, {});
    }
    auto result = reservation_->settle_aborted_after_cleanup();
    if (result.ok()) {
        reservation_.reset();
    }
    return result;
}

SegmentCapacitySnapshot RecordingSegment::capacity_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
}

}  // namespace rodaq::pipeline
