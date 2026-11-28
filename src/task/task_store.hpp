#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/task/config.hpp>
#include <rodaq/task/types.hpp>

#include "../persistence/atomic_file.hpp"
#include "operation_history.hpp"

namespace rodaq::task {

struct TaskStorePolicy {
    TaskConfigLimits config;
    HistoryPolicy history;
    std::size_t max_config_versions{64};
};

struct TaskMetadata {
    TaskId task_id;
    std::string active_config_version;
    std::uint64_t next_pipeline_id{1};
    std::uint64_t next_stream_id{1};
    // Stable stream_id -> stream_key bindings preventing rebind across Configure.
    std::map<StreamId, std::string> known_streams;
};

struct LoadedTask {
    TaskMetadata metadata;
    TaskConfig active_config;
};

// Durable storage for Task configuration and stable identities only:
//   <root>/tasks/<task-id>/meta.json
//   <root>/tasks/<task-id>/configs/<config-version>.json
// Create publishes <task-id>.tmp -> <task-id> atomically via RENAME_NOREPLACE.
// Startup cleans stale *.tmp directories and loads valid final task directories.
class TaskConfigStore {
public:
    static Result<TaskConfigStore> open(
        const std::filesystem::path& tasks_root, TaskStorePolicy policy = {},
        persistence::FileFaultHook faults = {});

    // Cleans stale *.tmp directories and loads all valid Tasks at startup.
    Result<std::vector<LoadedTask>> load_all();

    // Assigns Task/Pipeline/Stream IDs, generates UUIDv4 config_version,
    // writes immutable config + meta.json inside <task-id>.tmp, and atomically
    // publishes <task-id>.tmp -> <task-id>.
    Result<LoadedTask> create_task(TaskId task_id, const TaskConfig& input);

    // Validates stream_id stability, assigns new IDs as needed, writes immutable
    // configs/<new-uuid>.json, and atomically updates meta.json.
    Result<LoadedTask> update_config(const TaskMetadata& current, const TaskConfig& input);

    // Atomically removes <task-id> by renaming <task-id> -> <task-id>.tmp and
    // deleting the tree with bounded depth/entry limits.
    Result<void> remove_task(TaskId task_id);

private:
    TaskConfigStore(
        std::filesystem::path root, TaskStorePolicy policy,
        persistence::FileFaultHook faults) noexcept;
    std::filesystem::path root_;
    TaskStorePolicy policy_;
    persistence::FileFaultHook faults_;
};

}  // namespace rodaq::task
