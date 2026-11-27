#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/task/task_service.hpp>

#include "task_store.hpp"
#include "run_authorization.hpp"
#include "../execution_lane.hpp"
#include "../pipeline/stream_identity_store.hpp"

namespace rodaq::task {

enum class AdmissionRole { Acquisition, Storage };

struct AllowedPlugin {
    std::string plugin_id;
    std::string plugin_version;
    AdmissionRole role{AdmissionRole::Acquisition};
};

struct ResourceProfileAdmission {
    std::string name;
    std::uint64_t max_segment_physical_bytes{0};
};

enum class ServiceFileDomain {
    Directories,
    CreateReceipts,
    Task,
    StreamIdentity,
    Recovery,
    Retirement
};
enum class MaterializationStep { Allocated, Materializing, Config, Identity, Control, Completed };
enum class ResetStep { BeforeDelete, AfterDeleteBeforeRecovery, BeforeControlTerminal };
enum class RetireStep { BeforeTombstone, AfterTombstone, NamespaceCleanup };

struct ArtifactOperations {
    artifact::VolumeId volume_id;
    std::function<Result<std::vector<ArtifactId>>(TaskId, ControlGeneration)> reset_targets;
    std::function<Result<void>(
        ArtifactId, std::uint64_t, WallTime, std::chrono::steady_clock::time_point)>
        delete_for_reset;
    std::function<Result<void>(TaskId, ControlGeneration)> purge_quarantined_for_reset;
    std::function<Result<bool>(TaskId)> retire_eligible;
    std::function<Result<bool>(TaskId)> has_task_references;
};

struct RunInputs {
    std::vector<RunPipeline> pipelines;
    time::BootIdentity boot;
};

struct RuntimeRecoveryPolicy {
    std::uint32_t max_attempts{8};
    std::chrono::nanoseconds initial_backoff{std::chrono::milliseconds{100}};
    std::chrono::nanoseconds max_backoff{std::chrono::seconds{30}};
};

bool valid_runtime_recovery_policy(const RuntimeRecoveryPolicy& policy) noexcept;

enum class RunPurpose { Operation, VolumeRecovery, BootRecovery };

struct RunExecutionOptions {
    time::BootIdentity boot;
    pipeline::ReceiveClock sample{&time::sample_clocks};
    // Separate bounded Native domains, each with at least max_tasks workers
    // and queue slots. A blocked Task cannot consume another Task's capacity.
    // The resolver must supply independent actual Writer ownership per run.
    std::shared_ptr<ExecutionLane> preparation_lane;
    std::shared_ptr<ExecutionLane> cleanup_lane;
    // Optional dedicated per-Pipeline drain/cleanup domain so a blocking
    // Pipeline quiesce inside a multi-Pipeline Task cannot prevent sibling
    // healthy Pipelines from draining, sealing, and committing.
    std::shared_ptr<ExecutionLane> pipeline_cleanup_lane;
    std::chrono::nanoseconds native_timeout{std::chrono::seconds{5}};
    RecoveryPolicy recovery;
    RuntimeRecoveryPolicy runtime_recovery;
    // Composition resolves frozen Deployment/profile/Volume/Time facts and
    // durably reserves identities. It returns closed Core controllers only;
    // no Native creation, Prepare or Gate opening belongs in this resolver.
    std::function<Result<RunInputs>(
        const TaskControlRecord&, const TaskConfig&, pipeline::StreamIdentityStore&)>
        resolve;
    // Actual Artifact owner retains failed/uncertain commits. Success proves
    // durable Ledger + Catalog publication and transfer of retained charges.
    std::function<Result<void>(const pipeline::SealedPipelineSegment&)> commit;
    // Optional async-capable Commit handoff invoked with a post-commit hook
    // that runs on the Commit domain upon actual durable success (including
    // after caller observer timeout) so late commits settle Pipeline state.
    std::function<Result<void>(const pipeline::SealedPipelineSegment&, std::function<void()>)>
        commit_with_ack;
    std::function<Result<std::uint64_t>()> reserve_segment;
    // Invoked after durable Retire tombstone publication and proven actual
    // execution settlement so per-Task Writer lane slices can be reused.
    std::function<void(TaskId)> release_task_resources;
    ArtifactOperations artifacts;
};

struct ServiceOptions {
    // Prepared private state root. Namespace initialization/reconciliation is a
    // startup activity; public query methods never scan or perform filesystem I/O.
    std::filesystem::path root;
    std::shared_ptr<plugin::PluginManager> plugin_manager;
    // Composition retains this scheduling domain through explicit shutdown;
    // Service destruction never joins threads or cancels accepted work.
    std::shared_ptr<ExecutionLane> create_lane;
    // Separate actual Control publication capacity; queue >= max_tasks.
    // Do not populate this domain with permanent Native Writer loops.
    std::shared_ptr<ExecutionLane> control_lane;
    std::shared_ptr<const RunExecutionOptions> execution;
    std::vector<AllowedPlugin> allowed_plugins;
    std::vector<ResourceProfileAdmission> resource_profiles;
    std::vector<std::string> output_roots;
    std::size_t max_tasks{8};
    std::size_t max_retired_tombstones{1024};
    // Additional retained receipt capacity beyond active Task capacity. Pending
    // materialization never consumes an active catalog slot; total receipts
    // remain bounded by max_tasks + create_recovery_slots, including failures.
    std::size_t create_recovery_slots{8};
    std::size_t max_requests{32};
    std::size_t max_stream_identities{1024};
    TaskStorePolicy store;
    std::chrono::nanoseconds request_wait{std::chrono::seconds{30}};
    std::chrono::nanoseconds runtime_snapshot_max_age{std::chrono::seconds{2}};
    // Private deterministic persistence/crash seams, absent in production.
    std::function<int(ServiceFileDomain, persistence::FileStep)> file_faults;
    std::function<int(MaterializationStep)> materialization_faults;
    std::function<int(ResetStep)> reset_faults;
    std::function<int(RetireStep)> retire_faults;
};

}  // namespace rodaq::task
