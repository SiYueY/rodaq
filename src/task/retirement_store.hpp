#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/task/types.hpp>

#include "../persistence/atomic_file.hpp"

namespace rodaq::task {

struct TaskRetirementRecord {
    TaskId task_id;
    OperationId retire_operation_id;
    RequestIdentity request;
    std::string request_digest;
    ControlGeneration last_control_generation{0};
    std::string last_config_version;
    WallTime retired_at;
};

struct RetirementCollectProof {
    WallTime now;
    std::chrono::nanoseconds audit_window{0};
    bool active_namespace_removed{false};
    bool no_artifact_references{false};
    bool no_pending_maintenance{false};
};

Result<void> validate_retirement_record(const TaskRetirementRecord& record);
Result<std::string> encode_retirement_record(const TaskRetirementRecord& record);
Result<TaskRetirementRecord> decode_retirement_record(std::string_view bytes);

OperationSnapshot retirement_operation_snapshot(const TaskRetirementRecord& record);

// Exclusive owner of durable retired/<task_id>.json tombstones. Tombstone
// durability is the irreversible linearization point for Task retirement.
// Read queries inspect the reconciled durable cache without blocking on disk
// I/O; mutations serialize actual filesystem durability and fail closed on
// uncertain writes until reconciled.
class TaskRetirementStore {
public:
    static Result<std::unique_ptr<TaskRetirementStore>> open(
        const std::filesystem::path& directory, std::size_t max_tombstones,
        persistence::FileFaultHook faults = {});

    TaskRetirementStore(const TaskRetirementStore&) = delete;
    TaskRetirementStore& operator=(const TaskRetirementStore&) = delete;
    ~TaskRetirementStore() noexcept;

    Result<void> publish(const TaskRetirementRecord& record);
    Result<TaskRetirementRecord> load(TaskId task_id) const;
    Result<bool> contains(TaskId task_id) const;
    Result<std::vector<TaskRetirementRecord>> list() const;

    // Checks whether an idempotent Retire request matches an existing tombstone.
    // Returns matching retire_operation_id on exact key+digest match, Conflict on
    // same key with different digest, or NotFound if no matching key exists.
    Result<OperationId> lookup_request(
        TaskId task_id, const RequestIdentity& request, std::string_view request_digest) const;

    // Fail-closed tombstone GC: requires audit window expiry AND proven absence
    // of old active namespace, artifact/recovery references, and maintenance.
    Result<void> collect(TaskId task_id, const RetirementCollectProof& proof);

    Result<void> reconcile();

private:
    struct State;
    explicit TaskRetirementStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::task
