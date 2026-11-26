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
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rodaq/result.hpp>

namespace rodaq::server {

enum class CallDomain { Control, Query, Maintenance };

enum class AdmissionClass { Ordinary, Reserve };

enum class CallAdmissionFailure { PrincipalQuota, GlobalSaturation, ShuttingDown };

struct DomainMetrics {
    std::uint64_t queued{0};
    std::uint64_t active{0};
    std::uint64_t high_water_queued{0};
    std::uint64_t high_water_active{0};
    std::uint64_t rejected_principal{0};
    std::uint64_t rejected_global{0};
    std::uint64_t timed_out{0};
    std::uint64_t late_completed{0};
};

struct ExecutorMetrics {
    DomainMetrics control_ordinary;
    DomainMetrics control_reserve;
    DomainMetrics query_ordinary;
    DomainMetrics query_reserve;
    DomainMetrics maintenance;
};

struct ExecutorSubmitResult {
    bool admitted{false};
    CallAdmissionFailure failure{CallAdmissionFailure::GlobalSaturation};
};

// Bounded Core-call execution domains with physically separate ordinary and
// emergency reserve lanes. Observer timeout disconnects response publication
// only; active worker slot and captured state stay alive until actual return.
class ServiceCallExecutor {
public:
    static Result<std::unique_ptr<ServiceCallExecutor>> create(const ExecutionConfig& config);

    ServiceCallExecutor(const ServiceCallExecutor&) = delete;
    ServiceCallExecutor& operator=(const ServiceCallExecutor&) = delete;
    ~ServiceCallExecutor() noexcept;

    // work receives try_claim_response(): calling it returns true at most once
    // if the observer has not yet timed out, guaranteeing exactly-once HTTP
    // response publication even under concurrent timeout / Core completion.
    ExecutorSubmitResult submit(
        CallDomain domain, AdmissionClass admission, const std::string& principal,
        std::chrono::nanoseconds observer_timeout,
        std::function<void(const std::function<bool()>& try_claim_response)> work,
        std::function<void()> on_timeout);

    // Cancels unstarted queued work and waits up to deadline for active workers.
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

    ExecutorMetrics metrics() const;

private:
    struct Lane;
    struct QuotaState;
    struct TaskItem;

    explicit ServiceCallExecutor(ExecutionConfig config);
    std::shared_ptr<Lane>& select_lane(CallDomain domain, AdmissionClass admission) noexcept;
    static void worker_loop(
        const std::shared_ptr<Lane>& lane, const std::shared_ptr<QuotaState>& quota);
    void timer_loop();

    ExecutionConfig config_;
    std::shared_ptr<Lane> control_ordinary_;
    std::shared_ptr<Lane> control_reserve_;
    std::shared_ptr<Lane> query_ordinary_;
    std::shared_ptr<Lane> query_reserve_;
    std::shared_ptr<Lane> maintenance_;
    std::shared_ptr<QuotaState> quota_;

    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
    std::vector<std::shared_ptr<TaskItem>> pending_timers_;
    std::atomic<bool> stopping_{false};
    bool stopped_{false};
    std::thread timer_thread_;
};

}  // namespace rodaq::server
