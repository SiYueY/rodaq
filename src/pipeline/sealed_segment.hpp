#pragma once

#include <cstdint>
#include <memory>

#include <rodaq/storage/types.hpp>

#include "../artifact/manifest.hpp"

namespace rodaq::pipeline {

class RecordingSegment;

// Actual sealed handoff, independent of the Controller/Native execution API.
// Artifact owns hashing/Manifest/Ledger/Catalog work. Pipeline retains its
// commit obligation until the actual Artifact owner acknowledges settlement.
struct SealedPipelineSegment {
    std::shared_ptr<RecordingSegment> segment;
    storage::SegmentSealResult files;
    storage::PrepareConfig envelope;
    artifact::ManifestLimits manifest_limits;
    std::uint64_t work_id{0};
};

}  // namespace rodaq::pipeline
