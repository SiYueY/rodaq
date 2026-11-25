#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ledger_store.hpp"
#include "manifest.hpp"
#include "../time/playback_anchor_store.hpp"

namespace rodaq::task {
struct TaskControlRecord;
}

namespace rodaq::artifact {

enum class DeleteStep;

// Caller holds exclusive boot execution and the loaded durable Task Control.
// A greater Reset counter is irreversible invalidation, never a transient
// authorization error. Retain opaque trees under Task/generation-scoped names.
Result<void> quarantine_reset_invalidated_artifact(
    std::shared_ptr<ManagedVolume> volume, LedgerStore& ledger, std::uint64_t segment,
    const task::TaskControlRecord& control, persistence::FileFaultHook faults = {});

// Caller holds exclusive boot execution after proving a deterministic
// pre-commit failure (corrupt/missing/ambiguous tree or invalid binding).
// Moves any present .tmp/final tree to quarantine and removes the pre-commit
// Ledger row without ever promoting it to Committed.
Result<void> quarantine_failed_precommit_artifact(
    std::shared_ptr<ManagedVolume> volume, LedgerStore& ledger, std::uint64_t segment,
    persistence::FileFaultHook faults = {});

// Exclusive startup execution. Resume only a durable DeleteIntent; an existing
// Deleted row can prove absence but never authorize deleting reappeared files.
Result<void> recover_deleting_artifact(
    std::shared_ptr<ManagedVolume> volume, LedgerStore& ledger, std::uint64_t segment,
    const ManifestLimits& limits, std::function<int(DeleteStep)> faults = {});

struct RecoveredVolumeUsage {
    std::uint64_t physical_bytes{0};
    std::uint32_t artifact_slots{0};
};

// Exclusive startup inspection. Count verified committed bytes and every
// metadata row, including absent Deleted tombstones; other states require recovery.
// Does not publish Ledger authority, mutate files or adopt unknown directories.
Result<RecoveredVolumeUsage> inspect_committed_volume(
    const std::shared_ptr<ManagedVolume>& volume, const LedgerStore& ledger,
    const ManifestLimits& limits);

enum class QuarantineReason {
    ResetInvalidated,
    PrecommitFailure,
    LedgerAbsentFinal,
    LedgerAbsentStaging,
    DeletedArtifactReappeared,
    UnknownEntry
};

struct QuarantinedArtifact {
    std::string name;
    bool in_quarantine_dir{true};
    QuarantineReason reason{QuarantineReason::UnknownEntry};
    std::optional<TaskId> task_id;
    std::optional<ControlGeneration> control_generation;
    std::optional<std::uint64_t> reset_generation;
    std::optional<std::uint64_t> segment_id;
    std::uint64_t physical_bytes{0};
    bool bytes_complete{true};
};

struct VolumeRecoveryReport {
    std::uint64_t committed_bytes{0};
    std::uint64_t quarantined_bytes{0};
    std::uint32_t artifact_slots{0};
    std::uint32_t committed_count{0};
    std::uint32_t deleted_count{0};
    std::uint32_t corrupted_committed_count{0};
    std::uint32_t verification_required_count{0};
    std::vector<QuarantinedArtifact> quarantined;
    std::vector<std::uint64_t> corrupted_committed_segments;
    time::BootArtifactEvidence boot_evidence;
    bool usage_complete{true};
    StatusCode health{StatusCode::Ok};

    std::uint64_t total_physical_bytes() const noexcept {
        return committed_bytes + quarantined_bytes;
    }
};

// Exclusive startup reconciliation scan after Deleting and Committing records
// have settled. Classifies Committed, Deleted, corrupted Committed, Ledger-absent
// entries, and quarantine trees without ever adopting unknown files as Committed.
Result<VolumeRecoveryReport> inspect_volume_recovery(
    const std::shared_ptr<ManagedVolume>& volume, const LedgerStore& ledger,
    const ManifestLimits& limits, const time::BootIdentity& current_boot);

}  // namespace rodaq::artifact
