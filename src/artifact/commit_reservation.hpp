#pragma once

#include <cstdint>

#include "ledger_store.hpp"
#include "volume_capacity.hpp"

namespace rodaq::artifact {

// Actual ownership handoff after Storage Seal returns and Admission/candidates
// are drained. Storage bytes have been verified and charged; the remaining
// future exposure is exactly the pre-admitted Core Manifest bound. Moving this
// aggregate transfers responsibility, never releases a reservation.
struct CommitReservation {
    VolumeReservation volume;
    LedgerRegistration registration;
    std::uint64_t storage_bytes;
    std::uint64_t manifest_bound;
};

}  // namespace rodaq::artifact
