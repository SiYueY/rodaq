#pragma once

#include <cstdint>

#include "volume_capacity.hpp"

namespace rodaq::artifact {

// Ownership handoff after Storage Seal. storage_bytes is the reported size
// until verified by SealedFiles; accounting changes only from verified facts.
struct CommitReservation {
    VolumeReservation volume;
    std::uint64_t storage_bytes{0};
};

}  // namespace rodaq::artifact
