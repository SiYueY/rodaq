#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/task/types.hpp>

namespace rodaq {

enum class SnapshotFreshness { NotObserved, Fresh, Stale };

enum class RecordRejectionReason {
    GateClosed,
    ObsoleteInstance,
    StaleDescriptor,
    DescriptorNotReady,
    InvalidRecord,
    RecordTooLarge,
    ResourceExhausted
};

struct RecordRejectionCount {
    RecordRejectionReason reason{RecordRejectionReason::InvalidRecord};
    std::uint64_t records{0};
};

struct StreamRuntimeFacts {
    bool entry_ready{false};
    bool source_ready{false};
    std::uint64_t active_sources{0};
    // Core COPY service boundary observations, including rejected attempts.
    // Native drops before this boundary remain separate acquisition evidence.
    std::uint64_t received_records{0};
    std::uint64_t received_logical_bytes{0};
    std::optional<WallTime> last_receive_at;
    std::optional<bool> receiving;
    std::optional<double> received_records_per_second;
    std::optional<double> received_logical_bytes_per_second;
    std::uint64_t accepted_records{0};
    std::vector<RecordRejectionCount> rejections;
    std::uint64_t native_dropped_records{0};
    std::uint64_t native_overflow_records{0};
    bool evidence_complete{true};
};

struct StreamRuntimeSnapshot {
    StreamId stream_id{0};
    std::string stream_key;
    std::string name;
    std::optional<StreamRuntimeFacts> facts;
};

struct PipelineRuntimeFacts {
    std::string instance_id;
    ControlGeneration control_generation{0};
    std::optional<StatusCode> prepare_status;
    bool admission_open{false};
    bool quiescent{false};
    StatusCode writer_failure{StatusCode::Ok};
    bool transport_ready{false};
    bool acquisition_terminal_failure{false};
    std::uint64_t accepted_records{0};
    std::uint64_t written_records{0};
    std::uint64_t known_failed_records{0};
    std::uint64_t persistence_unknown_records{0};
    std::uint64_t unsettled_records{0};
    std::uint64_t queued_records{0};
    std::uint64_t queued_logical_bytes{0};
    std::uint32_t commit_items{0};
    DataImpact data_impact{DataImpact::None};
};

struct PipelineRuntimeSnapshot {
    PipelineId pipeline_id{0};
    bool required{false};
    std::optional<PipelineRuntimeFacts> facts;
    std::vector<StreamRuntimeSnapshot> streams;
};

// Immutable observations, never control authority. All rows are bounded by the
// accepted Task configuration. Missing facts differ from observed zero values.
struct TaskRuntimeSnapshot {
    TaskId task_id;
    std::string active_config_version;
    ControlGeneration control_generation{0};
    std::uint64_t snapshot_sequence{0};
    WallTime captured_at;
    SnapshotFreshness freshness{SnapshotFreshness::NotObserved};
    std::chrono::nanoseconds age{0};
    DataImpact data_impact{DataImpact::None};
    bool evidence_complete{true};
    std::vector<PipelineRuntimeSnapshot> pipelines;
    std::optional<std::chrono::nanoseconds> window;
    std::optional<double> accepted_records_per_second;
};

}  // namespace rodaq
