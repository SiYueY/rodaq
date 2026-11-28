#pragma once

#include <cstdint>

#include "volume_capacity.hpp"

namespace rodaq::artifact {

// Ownership handoff from Pipeline Writer to Commit lane after Storage Seal.
struct CommitReservation {
    VolumeReservation volume;
    std::uint64_t storage_bytes{0};
};

}  // namespace rodaq::artifact
