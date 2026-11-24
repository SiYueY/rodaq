#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

enum class ServiceFileDomain { Directories, CreateReceipts, Task, StreamIdentity, Recovery };
enum class MaterializationStep { Allocated, Materializing, Config, Identity, Control, Completed };

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

enum class RunPurpose { Operation, VolumeRecovery };

struct RunExecutionOptions {
    pipeline::ReceiveClock sample{&time::sample_clocks};
    // Separate bounded Native domains, each with at least max_tasks workers
    // and queue slots. A blocked Task cannot consume another Task's capacity.
    // The resolver must supply independent actual Writer ownership per run.
    std::shared_ptr<ExecutionLane> preparation_lane;
    std::shared_ptr<ExecutionLane> cleanup_lane;
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
    std::function<Result<std::uint64_t>()> reserve_segment;
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
};

}  // namespace rodaq::task
