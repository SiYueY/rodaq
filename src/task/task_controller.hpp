#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "control_record.hpp"
#include "config_identity.hpp"

namespace rodaq::pipeline {
class StreamIdentityStore;
}

namespace rodaq::task {

enum class ControlWorkKind { Publish, Reconcile };
enum class ControlPublicationKind { Intent, OperationUpdate };

struct ConfigPublication {
    AssignedConfig assigned;
    pipeline::StreamIdentityProjection projection;
    std::uint64_t expected_identity_revision{0};
};

struct ConfigureFacts {
    // Captured by the single Task control owner. Quiescence includes all old
    // instances and outstanding work, not merely the absence of an open Gate.
    const TaskConfig& active_config;
    const pipeline::StreamIdentityState& identities;
    bool quiescent{false};
    bool deployment_allowed{false};
};

struct LifecycleFacts {
    bool quiescent{false};
    bool execution_available{false};
};

// Immutable frame retained by the persistence worker until actual return.
// Observation timeout never calls persistence_completed or releases this frame.
struct ControlWork {
    std::uint64_t work_id{0};
    ControlWorkKind kind{ControlWorkKind::Publish};
    ControlPublicationKind publication{ControlPublicationKind::Intent};
    std::uint64_t expected_revision{0};
    std::shared_ptr<const TaskControlRecord> candidate;
    std::shared_ptr<const ConfigPublication> config_change;
};

struct ControlDecision {
    OperationId operation_id;
    bool durably_accepted{false};
    // Caller applies the bounded Pipeline execution fence before dispatching
    // work, on the same control turn. The Controller guards openings immediately.
    bool fence_required{false};
    std::shared_ptr<const ControlWork> work;
};

struct ControlResolution {
    OperationId operation_id;
    ControlPublicationKind publication{ControlPublicationKind::Intent};
    bool durable{false};
    Status status;
};

struct ControlCompletion {
    bool recognized{false};
    std::optional<ControlResolution> resolved;
    // Error notifications do not retire unresolved ownership while Protected.
    std::optional<ControlResolution> stop_resolution;
    std::shared_ptr<const ControlWork> next_work;
    bool persistence_protected{false};
};

class TaskStore;

struct ControlWorkResult {
    std::uint64_t work_id;
    Result<TaskControlRecord> authority;
    Status publication_failure;
    // Actual Control-selected facts after projection reconciliation, never
    // unaccepted candidate facts. Failure retains the publication for recovery.
    std::optional<pipeline::StreamIdentityState> identities{};
    std::shared_ptr<const TaskConfig> active_config{};
};

// Execute on actual persistence capacity only. This performs publish followed
// by fail-closed reconciliation if needed; Reconcile work never republishes.
// The scheduling owner retains both frame and Store through the actual return.
ControlWorkResult execute_control_work(
    TaskStore& store, const ControlWork& work, pipeline::StreamIdentityStore* identities = nullptr,
    const TaskConfigLimits& limits = {});

// A single Task's control owner. Called only on its serial control domain;
// does not perform filesystem/plugin calls, waits or thread management. One
// actual persistence flight plus one reserved superseding Stop slot is bounded.
// TaskService owns bounded public waiters, scheduling and snapshot publication.
class TaskController {
public:
    static Result<std::unique_ptr<TaskController>> create(
        TaskControlRecord durable, HistoryPolicy policy = {});

    TaskController(const TaskController&) = delete;
    TaskController& operator=(const TaskController&) = delete;

    Result<ControlDecision> request_lifecycle(
        OperationKind kind, const RequestIdentity& request, const ControlPrecondition& precondition,
        WallTime now, const LifecycleFacts& facts);
    Result<ControlDecision> request_configure(
        const TaskConfig& config, const RequestIdentity& request,
        const ControlPrecondition& precondition, WallTime now, const ConfigureFacts& facts,
        const TaskConfigLimits& limits);

    // Native/work ownership must already have been checked by the event owner.
    // Only the current Operation's mutable result fields may be published here.
    Result<std::shared_ptr<const ControlWork>> update_operation(const OperationSnapshot& result);

    // authority must be the worker's actual durable Store result: candidate on
    // success, or actual old/candidate authority after fail-closed reconciliation.
    // A failed observation is not an actual result. null/error protects the
    // controller and retains the unresolved publication for a fresh reconcile job.
    Result<ControlCompletion> persistence_completed(
        std::uint64_t work_id, Result<TaskControlRecord> authority,
        Status publication_failure = {});
    Result<std::shared_ptr<const ControlWork>> reconciliation_work();

    std::shared_ptr<const TaskControlRecord> durable_record() const noexcept { return durable_; }

    bool persistence_protected() const noexcept { return protected_; }

    bool run_is_current(OperationId operation, ControlGeneration generation) const noexcept;
    void authorize_boot_run() noexcept;

private:
    struct PendingStop {
        StoredOperation operation;
    };

    TaskController(std::shared_ptr<const TaskControlRecord> durable, HistoryPolicy policy) noexcept;
    Result<std::shared_ptr<const ControlWork>> make_intent(
        const TaskControlRecord& base, StoredOperation operation,
        std::shared_ptr<const ConfigPublication> config = {}) const;
    Result<std::shared_ptr<const ControlWork>> make_work(
        TaskControlRecord candidate, ControlPublicationKind publication,
        std::uint64_t expected_revision,
        std::shared_ptr<const ConfigPublication> config = {}) const;
    const StoredOperation* pending_request(std::string_view key) const noexcept;

    std::shared_ptr<const TaskControlRecord> durable_;
    const HistoryPolicy policy_;
    std::shared_ptr<const ControlWork> flight_;
    std::optional<PendingStop> stop_;
    bool protected_{false};
    bool opening_fenced_{true};
    bool actual_work_inflight_{false};
    Status unresolved_failure_;
};

}  // namespace rodaq::task
