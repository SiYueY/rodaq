#pragma once

#include <memory>
#include <string>

#include "commit_reservation.hpp"
#include "manifest.hpp"

namespace rodaq::artifact {

// One actual Commit-lane frame owns Volume/Commit capacity and registration.
// Its caller must retain authorized Task work until actual return, including
// after observer expiry. No Storage calls, Task locks or Catalog authority live
// here. A failed attempt requires recovery; it cannot be retried as new work.
class SegmentCommitter {
public:
    // Transfer only on success. Validation/allocation failure leaves both
    // actual file backing and registration/Volume capacity with the caller.
    static Result<std::unique_ptr<SegmentCommitter>> create(
        std::unique_ptr<SealedFiles>& files, const SegmentManifest& manifest,
        const ManifestLimits& limits, CommitReservation& reservation);
    SegmentCommitter(const SegmentCommitter&) = delete;
    SegmentCommitter& operator=(const SegmentCommitter&) = delete;

    Result<void> commit(LedgerStore& ledger);
    // Call only after Catalog publication (or verified quarantine/recovery).
    // Ledger success alone never removes retained actual disk/metadata charges.
    Result<ArtifactCharge> take_committed_charge();
    const LedgerRecord& ledger_record() const noexcept { return record_; }
    std::uint64_t storage_bytes() const noexcept { return files_->storage_bytes(); }
    std::uint64_t manifest_bytes() const noexcept { return encoded_manifest_.size(); }
    bool committed() const noexcept { return committed_; }

private:
    SegmentCommitter(
        std::unique_ptr<SealedFiles> files, std::string encoded, LedgerRecord record,
        CommitReservation reservation) noexcept;
    std::unique_ptr<SealedFiles> files_;
    std::string encoded_manifest_;
    LedgerRecord record_;
    CommitReservation reservation_;
    bool attempted_{false};
    bool committed_{false};
};

}  // namespace rodaq::artifact
