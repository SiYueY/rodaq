#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

#include "recovery_record.hpp"
#include "../persistence/atomic_file.hpp"

namespace rodaq::task {

struct RecoveryPolicy {
    RecoveryLimits limits;
    std::uint32_t max_publications_per_window{64};
    std::chrono::nanoseconds publication_window{std::chrono::seconds{1}};
};

// Independent, per-Task Recovery authority. All methods execute on bounded
// persistence capacity, never the Task control strand. A failed publication
// protects this owner until actual file/parent synchronization and reconciliation.
// Load/reconcile data is evidence, not permission to open a current Gate: Task
// must also check its durable Control generation, Operation and full Prepare set.
// Deployment owns the shared Recovery directory's bounded orphan-temp cleanup;
// this per-Task lease permits removing only temporaries from its own failed call.
class TaskRecoveryStore {
public:
    static Result<std::unique_ptr<TaskRecoveryStore>> open(
        const std::filesystem::path& recovery_directory, TaskId task_id, RecoveryPolicy policy = {},
        persistence::FileFaultHook faults = {});

    TaskRecoveryStore(const TaskRecoveryStore&) = delete;
    TaskRecoveryStore& operator=(const TaskRecoveryStore&) = delete;
    ~TaskRecoveryStore() noexcept;

    Result<TaskRecoveryRecord> load();
    Result<void> publish(const TaskRecoveryRecord& record, std::uint64_t expected_revision);
    Result<std::optional<TaskRecoveryRecord>> reconcile();

private:
    struct State;
    explicit TaskRecoveryStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::task
