#pragma once

#include <cstdint>
#include <memory>
#include <functional>

#include "ledger_store.hpp"
#include "manifest.hpp"

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

}  // namespace rodaq::artifact
