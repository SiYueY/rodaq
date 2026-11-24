#pragma once

#include <cstdint>
#include <memory>

#include "ledger_store.hpp"
#include "manifest.hpp"

namespace rodaq::artifact {

struct RecoveredVolumeUsage {
    std::uint64_t physical_bytes{0};
    std::uint32_t artifact_slots{0};
};

// Exclusive startup inspection only. Accepts fully verified committed trees;
// every other retained state remains a recovery diagnostic, never zero usage.
// Does not publish Ledger authority, mutate files or adopt unknown directories.
Result<RecoveredVolumeUsage> inspect_committed_volume(
    const std::shared_ptr<ManagedVolume>& volume, const LedgerStore& ledger,
    const ManifestLimits& limits);

}  // namespace rodaq::artifact
