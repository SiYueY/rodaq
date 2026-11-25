#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rodaq/result.hpp>

#include "security/auth.hpp"
#include "server_config.hpp"

namespace rodaq::server {

enum class CallDomain { Control, Query, Maintenance };

enum class AdmissionClass { Ordinary, Reserve };

enum class ServiceCallLifecycle {
    Queued,
    Running,
    Completed,
    TimedOutButInFlight,
    LateCompleted,
};

struct RequestContext {
    std::string request_id;
    ApiVersion api_version;
    std::string principal;
    ScopeSet scopes;
    std::chrono::steady_clock::time_point observer_deadline;
    bool is_mutation{false};
};

struct ExecutorMetrics {
    std::uint64_t control_active{0};
    std::uint64_t control_hwm{0};
    std::uint64_t query_active{0};
    std::uint64_t query_hwm{0};
    std::uint64_t rejected_saturation{0};
    std::uint64_t timed_out_in_flight{0};
    std::uint64_t late_completed{0};
};

// Bounded Core-call execution domains with physically isolated ordinary vs reserve lanes.
// Observer timeout never prematurely releases an active worker slot or captured backing.
class ServiceCallExecutor {
public:
    using WorkFn = std::function<void()>;
    using TimeoutFn = std::function<void()>;

    static Result<std::unique_ptr<ServiceCallExecutor>> create(const ExecutionConfig& config);

    ServiceCallExecutor(const ServiceCallExecutor&) = delete;
    ServiceCallExecutor& operator=(const ServiceCallExecutor&) = delete;
    ~ServiceCallExecutor() noexcept;

    // Submits work that executes `work(is_still_observed)` on the chosen domain lane.
    // If observer_deadline expires before `work` finishes, `on_timeout` runs once,
    // while the active worker slot remains held until `work` genuinely returns.
    Result<void> submit(
        CallDomain domain, AdmissionClass admission, RequestContext context,
        std::function<void(const std::function<bool()>& claim_response)> work,
        TimeoutFn on_timeout);

    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);
    ExecutorMetrics metrics() const noexcept;

private:
    struct Lane;
    struct CallState;

    ServiceCallExecutor() = default;
    void timer_loop() noexcept;

    std::unique_ptr<Lane> control_ordinary_;
    std::unique_ptr<Lane> control_reserve_;
    std::unique_ptr<Lane> query_ordinary_;
    std::unique_ptr<Lane> query_reserve_;
    std::unique_ptr<Lane> maintenance_;

    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
    std::vector<std::shared_ptr<CallState>> watched_;
    std::thread timer_thread_;
    std::atomic<bool> stopping_{false};
    bool stopped_{false};

    mutable std::atomic<std::uint64_t> rejected_saturation_{0};
    mutable std::atomic<std::uint64_t> timed_out_in_flight_{0};
    mutable std::atomic<std::uint64_t> late_completed_{0};
};

}  // namespace rodaq::server
