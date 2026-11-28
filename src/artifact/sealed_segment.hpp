#pragma once

#include <cstdint>

#include <rodaq/storage/types.hpp>

#include "commit_reservation.hpp"
#include "manifest.hpp"

namespace rodaq::artifact {

// Immutable move-only value handoff from Pipeline to Artifact after Storage Seal.
// Owns the captured SegmentManifest, sealed file list, Prepare envelope, Manifest
// limits, and a move-only, not-yet-verified reservation. This breaks any reverse
// Artifact -> Pipeline RecordingSegment dependency.
struct SealedSegment {
    std::uint64_t work_id{0};
    std::uint64_t segment_id{0};
    TaskId task_id{};
    SegmentManifest manifest;
    storage::SegmentSealResult files;
    storage::PrepareConfig envelope{};
    ManifestLimits manifest_limits;
    CommitReservation reservation;

    SealedSegment(const SealedSegment&) = delete;
    SealedSegment& operator=(const SealedSegment&) = delete;
    SealedSegment(SealedSegment&&) noexcept = default;
    SealedSegment& operator=(SealedSegment&&) = delete;
    ~SealedSegment() = default;
};

}  // namespace rodaq::artifact
