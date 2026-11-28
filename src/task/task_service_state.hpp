#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "operation_history.hpp"
#include "task_service.hpp"
#include "task_store.hpp"

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
    RequestSlot(RequestSlot&& other) noexcept
    : requests_(std::exchange(other.requests_, nullptr)) {}
    RequestSlot& operator=(RequestSlot&& other) noexcept {
        if (this != &other) {
            if (requests_) {
                requests_->fetch_sub(1, std::memory_order_release);
            }
            requests_ = std::exchange(other.requests_, nullptr);
        }
        return *this;
    }
    ~RequestSlot() noexcept {
        if (requests_) {
            requests_->fetch_sub(1, std::memory_order_release);
        }
    }
    RequestSlot(const RequestSlot&) = delete;
    RequestSlot& operator=(const RequestSlot&) = delete;

private:
    std::atomic<std::size_t>* requests_{nullptr};
};

struct PublishedTask {
    TaskSnapshot snapshot;
    std::shared_ptr<const TaskConfig> config;
    TaskRuntimeSnapshot runtime;
    std::chrono::steady_clock::time_point observed_at;
};

struct RunExecution {
    std::mutex mutex;
    std::condition_variable changed;
    TaskId task_id;
    std::string config_version;
    ControlGeneration control_generation{0};
    OperationId start_operation_id;
    std::uint64_t recording_base_ns{0};
    std::shared_ptr<const TaskConfig> config;
    std::vector<RunPipeline> pipelines;
    bool preparing{true};
    bool preparation_returned{false};
    bool opened{false};
    bool degraded{false};
    bool fenced{false};
    bool closing{false};
    bool rotating{false};
    bool volume_protected{false};
    StatusCode failure{StatusCode::Ok};
    DataImpact data_impact{DataImpact::None};

    void fence(std::optional<std::uint64_t> monotonic_ns = {}) noexcept;
    bool resources_settled();
    ExecutionCondition condition(DesiredState desired);
};

struct TaskEntry {
    TaskId id;
    std::mutex control_mutex;
    TaskMetadata metadata;
    std::shared_ptr<const TaskConfig> config;
    DesiredState desired_state{DesiredState::Stopped};
    ControlGeneration control_generation{1};
    std::uint64_t snapshot_sequence{1};
    std::uint64_t recording_base_ns{0};
    std::optional<OperationSnapshot> current_operation;
    OperationRegistry operations;
    std::shared_ptr<RunExecution> execution;
    std::shared_ptr<const PublishedTask> published;
    bool configuring{false};
    bool resetting{false};
    bool retiring{false};
    bool retired{false};
    bool protected_state{false};
    DataImpact last_data_impact{DataImpact::None};

    explicit TaskEntry(HistoryPolicy history) : operations(history) {}
};

Result<std::shared_ptr<const PublishedTask>> make_task_publication(
    const TaskEntry& entry, std::shared_ptr<const TaskConfig> config, std::uint64_t sequence,
    const PublishedTask* previous = nullptr);

Result<TaskRuntimeSnapshot> capture_task_runtime(
    const TaskEntry& entry, const TaskConfig& config, const TaskSnapshot& snapshot);

void project_runtime_rates(PublishedTask& current, const PublishedTask* previous) noexcept;

}  // namespace rodaq::task

namespace rodaq {

struct TaskService::State : std::enable_shared_from_this<TaskService::State> {
    std::shared_ptr<const task::ServiceOptions> options;
    std::vector<plugin::PluginInfo> installed;
    std::optional<task::TaskConfigStore> store;
    std::mutex store_mutex;
    std::mutex create_mutex;
    task::CreateRegistry create_registry;
    std::mutex catalog_mutex;
    std::vector<std::shared_ptr<task::TaskEntry>> entries;
    std::atomic<std::size_t> requests{0};
    std::atomic<std::size_t> active_jobs{0};
    std::atomic<bool> accepting{true};

    Result<task::RequestSlot> enter_request();
    Result<void> validate_deployment(const TaskConfig& config) const;
    std::shared_ptr<task::TaskEntry> find(TaskId id);
    void publish_locked(task::TaskEntry& entry) noexcept;

    void start_prepare(
        const std::shared_ptr<task::TaskEntry>& entry,
        const std::shared_ptr<task::RunExecution>& execution) noexcept;
    void start_close(
        const std::shared_ptr<task::TaskEntry>& entry,
        const std::shared_ptr<task::RunExecution>& execution,
        std::optional<OperationId> operation_id, ControlGeneration generation, bool is_pause,
        bool process_drain, bool internal_cleanup) noexcept;
    void start_reset(
        const std::shared_ptr<task::TaskEntry>& entry, OperationId operation_id,
        ControlGeneration generation) noexcept;
    void start_retire(
        const std::shared_ptr<task::TaskEntry>& entry, OperationId operation_id,
        ControlGeneration generation) noexcept;
};

}  // namespace rodaq
