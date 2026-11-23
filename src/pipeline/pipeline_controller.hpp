#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <rodaq/plugin/manager.hpp>
#include <rodaq/plugin/library.hpp>
#include <rodaq/plugin/instance.hpp>

#include "pipeline_writer.hpp"
#include "recording_instance.hpp"
#include "sealed_segment.hpp"
#include "../execution_lane.hpp"
#include "../artifact/ledger_store.hpp"

namespace rodaq::pipeline {

struct PipelineOptions {
    RecordingIdentity identity;
    PipelineConfig definition;
    std::vector<StreamPreparation> streams;
    InstanceLimits limits;
    storage::PrepareConfig envelope{};
    artifact::ManifestIdentity manifest;
    artifact::ManifestLimits manifest_limits;
    std::uint32_t max_prepared_descriptors{1024};
    std::shared_ptr<plugin::PluginManager> plugins;
    std::shared_ptr<ExecutionLane> writer_lane;
    std::shared_ptr<time::ClockCorrelation> clock;
    std::shared_ptr<artifact::ManagedVolume> volume;
    std::shared_ptr<artifact::VolumeCapacity> capacity;
    std::shared_ptr<artifact::LedgerStore> ledger;
    ReceiveClock receive_clock{&time::sample_clocks};
    std::chrono::nanoseconds native_timeout{std::chrono::seconds{5}};
};

struct PreparedPipeline {
    PipelineId pipeline_id{0};
    InstanceId instance_id;
    ControlGeneration control_generation{0};
    std::uint64_t prepared_monotonic{0};
    rodaq_system_boot_id_t boot_id{0};
    std::string effective_config_id{};
};

// Task supplies this only after durable full-set OpenAuthorization. Borrowed
// identity text lasts through this non-allocating, non-I/O Core gate operation.
struct OpenAuthorization {
    TaskId task_id;
    PipelineId pipeline_id{0};
    InstanceId instance_id;
    ControlGeneration control_generation{0};
    std::string_view effective_config_id;
    std::uint64_t prepared_monotonic{0};
    rodaq_system_boot_id_t boot_id{0};
    std::uint64_t opened_monotonic{0};
    std::uint64_t recording_base{0};
};

struct PipelineQuiescence {
    bool gate_closed{false};
    bool control_fenced{false};
    bool source_quiesced{false};
    bool storage_closed{false};
    bool segment_resources_settled{false};
    std::uint32_t control_inflight{0};
    std::uint32_t writer_inflight{0};
    std::uint32_t callbacks{0};
    std::uint64_t queued_records{0};
    std::uint64_t unsettled_records{0};
    std::uint64_t buffered_records{0};
    std::uint32_t unsettled_descriptors{0};
    std::uint32_t admission_candidates{0};
    std::uint32_t commit_inflight{0};
    bool settled{false};
};

struct PipelineSnapshot {
    RecordingIdentity identity;
    GateSnapshot gate;
    SegmentMetadataSnapshot records;
    RecordQueueSnapshot queue;
    PipelineQuiescence quiescence;
    bool prepared{false};
    // Immutable Plugin initialization inputs were rejected as InvalidArgument.
    // This does not classify later Prepare/Source/Writer failures.
    bool configuration_rejected{false};
    StatusCode failure{StatusCode::Ok};
    std::uint64_t segment_id{0};
    std::uint64_t artifact_work_id{0};
    std::uint64_t successor_segment_id{0};
    std::uint64_t segment_started_monotonic{0};
    SegmentCapacitySnapshot capacity;
};

class PipelineController {
public:
    static Result<std::shared_ptr<PipelineController>> create(PipelineOptions options);
    PipelineController(const PipelineController&) = delete;
    PipelineController& operator=(const PipelineController&) = delete;
    ~PipelineController() noexcept;

    // These blocking actual-work methods run on Native/cleanup execution
    // capacity. A caller observation timeout must retain their actual frame.
    Result<PreparedPipeline> prepare(plugin::Deadline deadline);
    GateOpenResult open_admission(const OpenAuthorization& authorization);
    Result<void> establish_control_fence() noexcept;
    Result<GateSnapshot> close_admission(ControlGeneration generation);
    Result<std::optional<SealedPipelineSegment>> drain(plugin::Deadline deadline);
    // Caller reserves a deployment-wide durable ID before entering. Repeating
    // an unfinished transition supplies the same successor ID. Admission of
    // the successor starts at the Gate cut; Native Writer stays serial, and
    // at most one old sealed handoff remains retained for actual Artifact ack.
    Result<std::optional<SealedPipelineSegment>> rotate(
        std::uint64_t successor_segment_id, plugin::Deadline deadline);
    Result<bool> rotation_due(const SegmentPolicy& policy, std::uint64_t now_monotonic) const;
    // Only the actual Artifact owner acknowledges Catalog/retained settlement.
    Result<void> acknowledge_artifact(std::uint64_t work_id, std::uint64_t segment_id);
    Result<void> quiesce(plugin::Deadline deadline);
    // Failure cleanup may close Native owners while retaining an unsealed
    // artifact obligation. Success still requires actual Artifact settlement.
    Result<void> cleanup(plugin::Deadline deadline);
    Result<void> acknowledge_deleted(std::uint64_t work_id, std::uint64_t segment_id);
    Result<PipelineSnapshot> snapshot() const;
    Result<CallbackFacts> callback_facts() const;
    bool matches_configuration(
        std::string_view config_version, std::string_view effective_config_id) const noexcept;

private:
    struct WriterDomain;
    struct State;
    explicit PipelineController(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> state_;
};

}  // namespace rodaq::pipeline
