#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <rodaq/artifact/artifact_service.hpp>

#include "commit_reservation.hpp"
#include "manifest.hpp"

namespace rodaq::artifact {

// One Commit-lane frame owns Volume capacity and sealed staging files.
// Validates sealed files, writes manifest.json, optionally runs power-loss
// fsync hardening, and performs same-parent atomic no-replace publication
// (<id>.tmp -> <id>).
class SegmentCommitter {
public:
    static Result<std::unique_ptr<SegmentCommitter>> create(
        std::unique_ptr<SealedFiles>& files, const SegmentManifest& manifest,
        const ManifestLimits& limits, CommitReservation& reservation,
        DurabilityProfile durability = DurabilityProfile::Normal);
    SegmentCommitter(const SegmentCommitter&) = delete;
    SegmentCommitter& operator=(const SegmentCommitter&) = delete;

    Result<void> commit();
    Result<ArtifactCharge> take_committed_charge();
    Result<void> abort_after_cleanup();
    std::uint64_t storage_bytes() const noexcept { return files_->storage_bytes(); }
    std::uint64_t manifest_bytes() const noexcept { return encoded_manifest_.size(); }
    bool committed() const noexcept { return committed_; }

private:
    SegmentCommitter(
        std::unique_ptr<SealedFiles> files, std::string encoded, CommitReservation reservation,
        DurabilityProfile durability) noexcept;
    std::unique_ptr<SealedFiles> files_;
    std::string encoded_manifest_;
    CommitReservation reservation_;
    DurabilityProfile durability_{DurabilityProfile::Normal};
    bool attempted_{false};
    bool committed_{false};
};

}  // namespace rodaq::artifact
