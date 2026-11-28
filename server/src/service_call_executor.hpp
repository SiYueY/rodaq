#pragma once

#include "config.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rodaq/result.hpp>

namespace rodaq::server {

enum class CallAdmissionFailure { GlobalSaturation, ShuttingDown };

struct ControlMetrics {
    std::uint64_t active{0};
    std::uint64_t queued{0};
    std::uint64_t high_water_active{0};
    std::uint64_t high_water_queued{0};
    std::uint64_t rejected{0};
    std::uint64_t timed_out{0};
    std::uint64_t late_completed{0};
};

struct ExecutorSubmitResult {
    bool admitted{false};
    CallAdmissionFailure failure{CallAdmissionFailure::GlobalSaturation};
};

// Bounded worker pool for blocking Task control mutations.
// Observer timeout releases the HTTP observer while retaining the active worker
// slot and captured backing until the underlying Core call actually returns.
class ControlExecutor {
public:
    static Result<std::unique_ptr<ControlExecutor>> create(const ExecutionConfig& config);

    ControlExecutor(const ControlExecutor&) = delete;
    ControlExecutor& operator=(const ControlExecutor&) = delete;
    ~ControlExecutor() noexcept;

    ExecutorSubmitResult submit(
        std::chrono::nanoseconds observer_timeout,
        std::function<void(const std::function<bool()>& try_claim_response)> work,
        std::function<void()> on_timeout);

    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

    ControlMetrics metrics() const;

private:
    struct TaskItem;
    struct SharedState;

    explicit ControlExecutor(ExecutionConfig config);
    static void worker_loop(const std::shared_ptr<SharedState>& state);
    void timer_loop();

    ExecutionConfig config_;
    std::shared_ptr<SharedState> state_;
    std::atomic<bool> stopping_{false};
    bool stopped_{false};
    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
    std::vector<std::shared_ptr<TaskItem>> pending_timers_;
    std::thread timer_thread_;
};

}  // namespace rodaq::server
