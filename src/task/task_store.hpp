#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <rodaq/task/config.hpp>

#include "control_record.hpp"
#include "../persistence/atomic_file.hpp"

namespace rodaq::task {

enum class TaskStoreMode { Materializing, Existing };

struct TaskStorePolicy {
    TaskConfigLimits config;
    HistoryPolicy history;
    std::size_t max_config_versions{64};
};

struct TaskConfigVersion {
    std::string version;
    TaskConfig config;
};

// One task's Config and Control authority. Composition bounds the number of
// owners and prepares durable directories before opening. All I/O methods run
// on persistence capacity, not the control strand or public snapshot queries.
class TaskStore {
public:
    static Result<std::unique_ptr<TaskStore>> open(
        const std::filesystem::path& task_directory, const std::filesystem::path& control_directory,
        TaskId task_id, TaskStoreMode mode, TaskStorePolicy policy = {},
        persistence::FileFaultHook faults = {});

    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;
    ~TaskStore() noexcept;

    Result<TaskConfigVersion> create_config(const TaskConfig& config);
    Result<TaskConfigVersion> load_config(std::string_view version);
    Result<TaskControlRecord> load_control();
    Result<void> publish_control(const TaskControlRecord& record, std::uint64_t expected_revision);
    Result<std::optional<TaskControlRecord>> reconcile();

private:
    struct State;
    explicit TaskStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::task
