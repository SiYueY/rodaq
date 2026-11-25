#pragma once

#include <rodaq/artifact/artifact_service.hpp>

#include "task_service.hpp"
#include "task_controller.hpp"
#include "create_store.hpp"
#include "retirement_store.hpp"
#include "../pipeline/stream_identity_store.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>

namespace rodaq::task {

inline WallTime control_wall_now() {
    return std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
}

template <typename T>
struct Reply {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<Result<T>> result;

    void complete(Result<T> value) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        // Merged callers observe one immutable result. A later terminal-status
        // failure cannot retract an already durable Accepted response.
        if (result) {
            return;
        }
        result.emplace(std::move(value));
        ready.notify_all();
    }

    Result<T> wait(std::chrono::nanoseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, timeout, [&] { return result.has_value(); })) {
            return Status(StatusCode::DeadlineExceeded, {});
        }
        return *result;
    }
};

class RequestSlot {
public:
    explicit RequestSlot(std::atomic<std::size_t>& requests) noexcept : requests_(&requests) {}

    ~RequestSlot() noexcept { requests_->fetch_sub(1, std::memory_order_release); }
    RequestSlot(const RequestSlot&) = delete;
    RequestSlot& operator=(const RequestSlot&) = delete;

private:
    std::atomic<std::size_t>* requests_;
};

struct PublishedTask {
    TaskSnapshot snapshot;
    std::shared_ptr<const task::TaskControlRecord> control;
    std::shared_ptr<const TaskConfig> config;
    TaskRuntimeSnapshot runtime;
    std::chrono::steady_clock::time_point observed_at;
};

struct CleanupWork {
    std::uint64_t work_id{0};
    std::shared_ptr<Work> frame;
};

struct RunExecution {
    std::mutex registration;
    std::condition_variable returned;
    bool fenced{false};
    bool preparation_returned{false};
    bool preparing{true};
    bool draining{false};
    bool rotating{false};
    bool rotating_native{false};
    bool result_ready{false};
    bool ended_pending{false};
    bool recovery_exhausted{false};
    RunPurpose purpose{RunPurpose::Operation};
    bool result_partial{false};
    DataImpact result_impact{DataImpact::None};
    std::shared_ptr<RunAuthorization> authorization;
    std::vector<RunPipeline> pipelines;
    time::BootIdentity boot;
    pipeline::ReceiveClock sample{&time::sample_clocks};
    std::shared_ptr<const TaskControlRecord> control;
    std::shared_ptr<const TaskConfig> config;
    StatusCode failure{StatusCode::Ok};

    void fence(std::optional<std::uint64_t> monotonic_ns = {}) noexcept;
    bool resources_settled();
    ExecutionCondition condition(DesiredState desired);
};

struct RetentionWork {
    ArtifactId artifact;
    ControlGeneration generation;
    std::uint64_t committed_recording_end_ns{0};
};

struct VolumeRecovery {
    ControlGeneration generation{0};
    std::uint32_t attempts{0};
    std::uint64_t not_before{0};
};

enum class BootAction {
    None,
    ResumeReset,
    ResumeRunOperation,
    AutoResumeInterrupted,
    SettleInterruptedClose
};

struct TaskEntry {
    TaskId id;
    std::mutex control_mutex;
    std::unique_ptr<task::TaskStore> store;
    std::unique_ptr<pipeline::StreamIdentityStore> identities;
    pipeline::StreamIdentityState identity;
    std::unique_ptr<task::TaskController> controller;
    std::shared_ptr<TaskRecoveryStore> recovery;
    // Serializes actual Recovery I/O only, never taken by Task control/Open.
    std::mutex recovery_execution;
    std::shared_ptr<RunExecution> execution;
    std::optional<VolumeRecovery> volume_recovery;
    std::shared_ptr<const PublishedTask> published;
    BootAction boot_action{BootAction::None};
    bool quiescent{false};
    bool resetting{false};
    bool persisting{false};
    // Retained until actual deletion settles, including uncertain publication.
    // Configure cannot change policy while this original-generation work exists.
    std::optional<RetentionWork> retention_work;
    bool artifact_maintenance_active{false};
    // Failed dispatch only; an actual ended result is replayed, never re-issued.
    bool ready_to_submit{false};
    std::uint32_t closing{0};
    // One actual Native cleanup per Task and one deferred superseding flight.
    // Failed lane admission retains ready ownership for maintenance retry.
    std::uint64_t cleanup_active_id{0};
    CleanupWork cleanup_ready;
    CleanupWork cleanup_pending;
    std::optional<OperationId> closed_acceptance;
    std::shared_ptr<const task::ControlWork> control_work;
    std::shared_ptr<Reply<OperationId>> operation_reply;
    std::optional<OperationId> stop_id;
    std::shared_ptr<Reply<OperationId>> stop_reply;
    std::optional<task::ControlWorkResult> pending_result;
};

Result<std::shared_ptr<const PublishedTask>> make_task_publication(
    const TaskEntry& entry, std::shared_ptr<const TaskConfig> config, std::uint64_t sequence,
    std::shared_ptr<const TaskControlRecord> control = {});

Result<TaskRuntimeSnapshot> capture_task_runtime(
    const TaskEntry& entry, const TaskConfig& config, const TaskSnapshot& snapshot);

void project_runtime_rates(PublishedTask& current, const PublishedTask* previous) noexcept;

std::optional<OperationSnapshot> completed_configuration(const TaskControlRecord& control);

}  // namespace rodaq::task

namespace rodaq {

struct TaskService::State : std::enable_shared_from_this<TaskService::State> {
    std::shared_ptr<const task::ServiceOptions> options;
    std::vector<plugin::PluginInfo> installed;
    std::unique_ptr<task::TaskCreateStore> creates;
    std::unique_ptr<task::TaskRetirementStore> retirements;
    std::mutex catalog_mutex;
    std::vector<std::shared_ptr<task::TaskEntry>> entries;
    std::atomic<std::size_t> requests{0};
    std::atomic<bool> accepting{true};
    bool reconcile_creates{false};

    persistence::FileFaultHook file_hook(task::ServiceFileDomain domain) const;
    Result<void> step(task::MaterializationStep value) const;
    Result<void> directory(const std::filesystem::path& path) const;
    Result<void> validate_deployment(const TaskConfig& config) const;
    std::shared_ptr<task::TaskEntry> find(TaskId id);
    Result<void> insert(std::shared_ptr<task::TaskEntry> entry);
    Result<std::shared_ptr<task::TaskEntry>> materialize(
        TaskId id, const task::AssignedConfig* initial);
    Result<TaskId> create(const TaskConfig& config, const RequestIdentity& request);
    Result<void> enqueue_cleanup(
        const std::shared_ptr<task::TaskEntry>& entry, std::shared_ptr<Work> body,
        bool allow_defer = true);
    Result<void> retry_cleanup(const std::shared_ptr<task::TaskEntry>& entry);
    void cleanup_failed_pipelines(const std::shared_ptr<task::TaskEntry>& entry) noexcept;
    void start_execution(
        const std::shared_ptr<task::TaskEntry>& entry,
        task::RunPurpose purpose = task::RunPurpose::Operation,
        std::uint64_t now_monotonic = 0) noexcept;
    bool cleanup_failed_prepare(
        const std::shared_ptr<task::TaskEntry>& entry,
        const std::shared_ptr<task::RunExecution>& execution, StatusCode failure) noexcept;
    void finish_execution(
        const std::shared_ptr<task::TaskEntry>& entry, OperationId operation,
        ControlGeneration generation, StatusCode failure, bool partial = false,
        DataImpact impact = DataImpact::None) noexcept;
    void close_execution(
        const std::shared_ptr<task::TaskEntry>& entry,
        std::shared_ptr<task::Reply<OperationId>> reply,
        std::shared_ptr<const task::TaskControlRecord> accepted) noexcept;
    void execute_reset(const std::shared_ptr<task::TaskEntry>& entry) noexcept;
    void execute_retire(
        const std::shared_ptr<task::TaskEntry>& entry,
        std::shared_ptr<task::Reply<OperationId>> reply,
        std::shared_ptr<const task::TaskControlRecord> accepted) noexcept;
    Result<void> cleanup_retired_task_files(TaskId task_id);

    struct ResetJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        void run() noexcept override;
    };

    struct CreateWork final : Work {
        std::shared_ptr<State> owner;
        TaskConfig config;
        RequestIdentity request;
        std::shared_ptr<task::Reply<TaskId>> reply;
        void run() noexcept override;
    };

    struct CleanupFrame final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        std::shared_ptr<Work> body;
        std::uint64_t work_id{0};
        void run() noexcept override;
    };

    struct FailedPipelineCleanup final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::RunExecution> execution;
        void run() noexcept override;
    };

    struct ControlJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        std::shared_ptr<task::Reply<OperationId>> reply;
        std::shared_ptr<const task::ControlWork> frame;
        bool replay{false};
        void run() noexcept override;
    };

    struct PrepareJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        std::shared_ptr<task::RunExecution> execution;
        void run() noexcept override;
    };

    struct CloseJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        std::shared_ptr<task::RunExecution> execution;
        std::shared_ptr<const task::TaskControlRecord> control;
        std::shared_ptr<task::Reply<OperationId>> reply;
        bool internal_cleanup{false};
        bool process_drain{false};
        StatusCode preparation_failure{StatusCode::Ok};
        void run() noexcept override;
    };

    struct RotateJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        std::shared_ptr<task::RunExecution> execution;
        std::uint64_t now_monotonic{0};
        void run() noexcept override;
    };
};

}  // namespace rodaq
