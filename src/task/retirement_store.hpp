#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rodaq/task/types.hpp>

#include "../persistence/atomic_file.hpp"

namespace rodaq::task {

struct RetirementTombstone {
    TaskId task_id;
    OperationId operation_id;
    std::string request_key;
    std::string request_digest;
    ControlGeneration final_control_generation{0};
    WallTime accepted_at;
    WallTime retired_at;
    bool cleanup_completed{false};
};

// Sole durable authority for retired Task identity non-resurrection.
// Loaded BEFORE ordinary Task Boot Reconciliation; a valid tombstone permanently
// prevents re-materialization of its TaskId into the active catalog.
class TaskRetirementStore {
public:
    static Result<std::unique_ptr<TaskRetirementStore>> open(
        const std::filesystem::path& directory, std::size_t max_tombstones = 4096,
        persistence::FileFaultHook faults = {});

    TaskRetirementStore(const TaskRetirementStore&) = delete;
    TaskRetirementStore& operator=(const TaskRetirementStore&) = delete;
    ~TaskRetirementStore() noexcept;

    Result<void> publish(const RetirementTombstone& tombstone);
    Result<void> mark_cleanup_completed(TaskId task_id);
    Result<void> collect(TaskId task_id);

    bool is_retired(TaskId task_id) const noexcept;
    std::optional<RetirementTombstone> find(TaskId task_id) const;
    std::vector<RetirementTombstone> all() const;
    std::size_t count() const noexcept;

private:
    struct State;
    explicit TaskRetirementStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::task
