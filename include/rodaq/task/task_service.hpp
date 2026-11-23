#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/task/types.hpp>
#include <rodaq/task/runtime.hpp>

namespace rodaq {
namespace daq {
class Composition;
}
namespace artifact {
class ArtifactCatalog;
}
namespace task {
struct ServiceOptions;
}

struct TaskSnapshot {
    TaskId task_id;
    std::string active_config_version;
    DesiredState desired_state{DesiredState::Stopped};
    ControlGeneration control_generation{0};
    std::uint64_t reset_generation{0};
    std::optional<OperationSnapshot> current_operation;
    ExecutionCondition execution_condition{ExecutionCondition::Protected};
    std::uint64_t snapshot_sequence{0};
    WallTime captured_at;
    DataImpact data_impact{DataImpact::None};
};

struct TaskSummary {
    TaskSnapshot task;
    std::optional<double> accepted_records_per_second;
    std::optional<std::chrono::nanoseconds> commit_lag;
    SnapshotFreshness freshness{SnapshotFreshness::NotObserved};
    DataImpact data_impact{DataImpact::None};
};

struct TaskPage {
    std::vector<TaskSummary> items;
    bool has_more{false};
    std::string next_cursor;
};

class TaskService {
public:
    // Composition supplies immutable Deployment admissions and execution lanes.
    static Result<std::unique_ptr<TaskService>> open(task::ServiceOptions options);
    TaskService(const TaskService&) = delete;
    TaskService& operator=(const TaskService&) = delete;
    ~TaskService() noexcept;

    Result<TaskId> create_task(const TaskConfig& config, const RequestIdentity& request);
    Result<OperationId> configure(
        TaskId task_id, const TaskConfig& config, const RequestIdentity& request,
        const ControlPrecondition& precondition);
    Result<OperationId> start(
        TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition);
    Result<OperationId> pause(
        TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition);
    Result<OperationId> resume(
        TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition);
    Result<OperationId> stop(
        TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition);

    Result<TaskSnapshot> get_task(TaskId task_id) const;
    Result<TaskRuntimeSnapshot> get_runtime_snapshot(TaskId task_id) const;
    Result<TaskConfig> get_active_config(TaskId task_id) const;
    Result<TaskPage> list_tasks(const PageRequest& request) const;
    Result<OperationSnapshot> get_operation(TaskId task_id, OperationId operation_id) const;
    Result<OperationPage> list_operations(TaskId task_id, const PageRequest& request) const;

private:
    friend class daq::Composition;
    Result<void> close_admission();
    Result<void> drain_shutdown(bool pending_only = false);
    Result<void> refresh_execution_facts(bool protect = false);
    Result<void> schedule_rotation(std::uint64_t now_monotonic);
    Result<void> apply_retention(artifact::ArtifactCatalog& catalog);
    bool shutdown_settled() const;
    struct State;
    explicit TaskService(std::shared_ptr<State> state) noexcept;
    Result<OperationId> lifecycle(
        TaskId task_id, OperationKind kind, const RequestIdentity& request,
        const ControlPrecondition& precondition);
    std::shared_ptr<State> state_;
};

}  // namespace rodaq
