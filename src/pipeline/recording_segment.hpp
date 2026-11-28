#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <rodaq/storage/types.hpp>

#include "../artifact/commit_reservation.hpp"
#include "../artifact/volume_capacity.hpp"
#include "instance_id.hpp"
#include "segment_metadata.hpp"

namespace rodaq::pipeline {

struct SegmentCapacitySnapshot {
    std::uint64_t segment_id{0};
    std::uint64_t reserved_bytes{0};
    std::uint64_t actual_bytes{0};
    std::uint64_t input_bytes{0};
    std::uint64_t records{0};
    std::uint32_t candidates{0};
    bool writable{false};
    bool sealed{false};
    StatusCode admission_failure{StatusCode::Ok};
};

class RecordingSegment;

// RAII pre-admission candidate guard ensuring closed segments wait for every
// in-flight candidate to settle before sealing.
class RecordCandidate {
public:
    RecordCandidate(const RecordCandidate&) = delete;
    RecordCandidate& operator=(const RecordCandidate&) = delete;
    RecordCandidate(RecordCandidate&& other) noexcept;
    RecordCandidate& operator=(RecordCandidate&&) = delete;
    ~RecordCandidate() noexcept;

    Result<void> transfer();

private:
    friend class RecordingSegment;
    RecordCandidate(std::shared_ptr<RecordingSegment> owner, std::uint64_t input_bytes) noexcept;
    std::shared_ptr<RecordingSegment> owner_;
    std::uint64_t input_bytes_{0};
};

// Owns one Segment's coarse VolumeReservation + SegmentMetadata.
// Replaces per-record CapacityModel expansion with coarse SegmentReservation,
// actual physical size observation, and Volume watermark enforcement.
class RecordingSegment : public std::enable_shared_from_this<RecordingSegment> {
public:
    static Result<std::shared_ptr<RecordingSegment>> prepare(
        artifact::ManifestIdentity identity, std::vector<StreamId> streams,
        const storage::PrepareConfig& envelope, artifact::ManifestLimits manifest_limits,
        artifact::VolumeCapacity& volume, std::uint64_t reservation_bytes,
        std::uint64_t now_monotonic_ns);

    Result<RecordCandidate> reserve_record(
        std::uint64_t logical_bytes, std::uint64_t now_monotonic_ns);
    Result<void> observe_usage(const storage::SegmentUsage& usage);
    void mark_effects_possible() noexcept;
    void invalidate_usage();
    void close_admission();
    Result<artifact::CommitReservation> handoff_sealed(std::uint64_t storage_bytes);
    Result<void> settle_aborted_after_cleanup();
    SegmentCapacitySnapshot capacity_snapshot() const;

    SegmentMetadata metadata;
    const TaskId task_id;
    const PipelineId pipeline_id;
    const InstanceId instance_id;
    const artifact::VolumeId volume_id;

private:
    friend class RecordCandidate;
    RecordingSegment(
        SegmentMetadata facts, storage::PrepareConfig envelope, std::uint64_t manifest_reserve,
        artifact::VolumeReservation reservation, std::uint64_t reserved_bytes, InstanceId instance,
        artifact::VolumeId volume, TaskId task, PipelineId pipeline) noexcept;

    mutable std::mutex mutex_;
    const storage::PrepareConfig envelope_;
    const std::uint64_t manifest_reserve_{0};
    std::optional<artifact::VolumeReservation> reservation_;
    SegmentCapacitySnapshot capacity_;
};

}  // namespace rodaq::pipeline
