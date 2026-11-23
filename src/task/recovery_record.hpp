#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/task/types.hpp>

#include "../artifact/managed_volume.hpp"
#include "../pipeline/instance_id.hpp"

namespace rodaq::task {

// Full boot identity makes monotonic boundaries unambiguous across reboot.
struct RecoveryBoundary {
    std::string boot_uuid;
    std::uint64_t monotonic_ns{0};
};

enum class RunPhase { Prepared, OpenAuthorized, Ended };

struct AuthorizedPipeline {
    PipelineId pipeline_id{0};
    pipeline::InstanceId instance_id;
    std::string effective_config_id;
    RecoveryBoundary prepared_boundary;
    bool continuation_permission{false};
    bool opened_observed{false};
    std::optional<RecoveryBoundary> ended_boundary;
};

struct RunRecovery {
    OperationId operation_id;
    ControlGeneration control_generation{0};
    RunPhase phase{RunPhase::Prepared};
    std::vector<AuthorizedPipeline> authorized_pipelines;
    std::optional<RecoveryBoundary> authorization_boundary;
};

struct PipelineRecovery {
    ControlGeneration control_generation{0};
    AuthorizedPipeline instance;
};

struct CleanupIntent {
    PipelineId pipeline_id{0};
    pipeline::InstanceId instance_id;
    ControlGeneration control_generation{0};
};

struct VolumeResetProgress {
    artifact::VolumeId volume_id;
    // Actual durable deletion, Ledger settlement and Catalog acknowledgement;
    // these are execution checkpoints, never a substitute for Control reset intent.
    bool files_removed{false};
    bool ledger_settled{false};
    bool catalog_settled{false};
};

struct ResetProgress {
    OperationId operation_id;
    std::uint64_t reset_generation{0};
    std::vector<VolumeResetProgress> volumes;
};

struct TaskRecoveryRecord {
    TaskId task_id;
    std::uint64_t record_revision{1};
    ControlGeneration observed_control_generation{0};
    std::string active_config_version;
    // Proven cumulative Task recording floor. A failed clock cannot add its
    // unknown tail. Missing legacy evidence is not zero and needs reconciliation.
    std::optional<std::uint64_t> recording_time_ns;
    // Verified committed progress retained before deleting its artifact proof.
    // This is independent of silent Open time and unknown crash tails.
    std::optional<std::uint64_t> committed_recording_end_ns;
    std::optional<RunRecovery> run_operation;
    std::vector<PipelineRecovery> pipelines;
    std::optional<ResetProgress> reset_progress;
    std::vector<CleanupIntent> cleanup_intents;
};

struct RecoveryLimits {
    std::size_t max_bytes{256 * 1024};
    std::size_t max_pipelines{64};
    std::size_t max_cleanup_intents{128};
    std::size_t max_reset_volumes{32};
};

bool valid_recovery_limits(const RecoveryLimits& limits) noexcept;
Result<void> validate_recovery_record(
    const TaskRecoveryRecord& record, const RecoveryLimits& limits);
Result<std::string> encode_recovery_record(
    const TaskRecoveryRecord& record, const RecoveryLimits& limits);
Result<TaskRecoveryRecord> decode_recovery_record(
    std::string_view bytes, const RecoveryLimits& limits);

// Version ordering belongs to Recovery authority; this never interprets or
// rewrites Task Control intent. Caller checks current Control/Operation guards.
Result<void> recovery_successor(const TaskRecoveryRecord& previous, const TaskRecoveryRecord& next);

}  // namespace rodaq::task
