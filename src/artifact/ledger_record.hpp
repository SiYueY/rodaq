#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rodaq/task/types.hpp>

#include "managed_volume.hpp"
#include "../time/wall_time.hpp"

namespace rodaq::artifact {

enum class LedgerState { Committing, Committed, Deleting, Deleted };

struct LedgerRecord {
    std::uint64_t segment_id{0};
    LedgerState state{LedgerState::Committing};
    TaskId task_id;
    PipelineId pipeline_id{0};
    VolumeId volume_id;
    std::string managed_relative_path;
    std::string manifest_digest;
    ControlGeneration control_generation{0};
    std::uint64_t reset_generation_at_commit{0};
    time::WallTime commit_started_at;
    std::optional<time::WallTime> committed_at;
    std::optional<std::uint64_t> delete_generation;
    std::optional<std::string> delete_reason;
    std::optional<time::WallTime> deleted_at;
};

// Private stable spelling, decimal u64 fields and domain-specific JCS integrity.
// Nullable lifecycle fields have one representation and no inferred defaults.
Result<void> validate_ledger_record(const LedgerRecord& record);
Result<std::string> encode_ledger_record(const LedgerRecord& record);
Result<LedgerRecord> decode_ledger_record(std::string_view bytes);
Result<void> validate_ledger_transition(const LedgerRecord& before, const LedgerRecord& after);

}  // namespace rodaq::artifact
