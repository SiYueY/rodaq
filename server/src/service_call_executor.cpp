#include "service_call_executor.hpp"

#include <algorithm>
#include <utility>

namespace rodaq::server {

struct ServiceCallExecutor::Lane {
    std::uint32_t worker_count{1};
    std::uint32_t max_queue{8};
    std::size_t max_per_principal{0};
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::deque<std::shared_ptr<TaskItem>> queue;
    std::uint64_t active{0};
    DomainMetrics stats;
    bool stopping{false};
    std::vector<std::thread> workers;
};

struct ServiceCallExecutor::QuotaState {
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::size_t> principal_inflight;

    void release(const std::string& principal) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = principal_inflight.find(principal);
        if (it != principal_inflight.end() && --it->second == 0) {
            principal_inflight.erase(it);
        }
    }
};

struct ServiceCallExecutor::TaskItem {
    std::shared_ptr<Lane> lane;
    std::string principal;
    bool counts_principal{false};
    std::chrono::steady_clock::time_point deadline;
    std::function<void(const std::function<bool()>& try_claim_response)> work;
    std::function<void()> on_timeout;
    std::mutex state_mutex;
    bool started{false};
    bool completed{false};
    bool timed_out{false};
    bool response_claimed{false};
    bool cancelled_before_start{false};
};

ServiceCallExecutor::ServiceCallExecutor(ExecutionConfig config)
: config_(config), quota_(std::make_shared<QuotaState>()) {}

ServiceCallExecutor::~ServiceCallExecutor() noexcept {
    if (!stopped_) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

Result<std::unique_ptr<ServiceCallExecutor>> ServiceCallExecutor::create(
    const ExecutionConfig& config) {
    try {
        std::unique_ptr<ServiceCallExecutor> exec(new ServiceCallExecutor(config));

        auto init_lane = [&](std::uint32_t workers, std::uint32_t max_queue,
                             std::size_t per_principal) {
            auto lane = std::make_shared<Lane>();
            lane->worker_count = workers;
            lane->max_queue = max_queue;
            lane->max_per_principal = per_principal;
            lane->workers.reserve(workers);
            for (std::uint32_t i = 0; i < workers; ++i) {
                lane->workers.emplace_back(
                    [lane, quota = exec->quota_] { worker_loop(lane, quota); });
            }
            return lane;
        };

        const std::size_t ctrl_per_principal = std::max<std::size_t>(4, config.control_queue / 2);
        const std::size_t query_per_principal = std::max<std::size_t>(8, config.query_queue / 2);
        exec->control_ordinary_ =
            init_lane(config.control_workers, config.control_queue, ctrl_per_principal);
        exec->control_reserve_ =
            init_lane(config.control_reserve_workers, config.control_reserve_queue, 0);
        exec->query_ordinary_ =
            init_lane(config.query_workers, config.query_queue, query_per_principal);
        exec->query_reserve_ =
            init_lane(config.query_reserve_workers, config.query_reserve_queue, 0);
        exec->maintenance_ = init_lane(config.maintenance_workers, config.maintenance_queue, 0);
        exec->timer_thread_ = std::thread([raw = exec.get()] { raw->timer_loop(); });
        return exec;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "failed to allocate ServiceCallExecutor");
    }
}

std::shared_ptr<ServiceCallExecutor::Lane>& ServiceCallExecutor::select_lane(
    CallDomain domain, AdmissionClass admission) noexcept {
    if (domain == CallDomain::Control) {
        return admission == AdmissionClass::Reserve ? control_reserve_ : control_ordinary_;
    }
    if (domain == CallDomain::Query) {
        return admission == AdmissionClass::Reserve ? query_reserve_ : query_ordinary_;
    }
    return maintenance_;
}

ExecutorSubmitResult ServiceCallExecutor::submit(
    CallDomain domain, AdmissionClass admission, const std::string& principal,
    std::chrono::nanoseconds observer_timeout,
    std::function<void(const std::function<bool()>& try_claim_response)> work,
    std::function<void()> on_timeout) {
    if (stopping_.load(std::memory_order_acquire)) {
        return {false, CallAdmissionFailure::ShuttingDown};
    }
    std::shared_ptr<Lane>& lane = select_lane(domain, admission);
    const bool enforce_principal =
        admission == AdmissionClass::Ordinary && lane->max_per_principal > 0 && !principal.empty();
    if (enforce_principal) {
        bool exceeded = false;
        {
            std::lock_guard<std::mutex> qlock(quota_->mutex);
            auto& count = quota_->principal_inflight[principal];
            if (count >= lane->max_per_principal) {
                exceeded = true;
            } else {
                ++count;
            }
        }
        if (exceeded) {
            std::lock_guard<std::mutex> llock(lane->mutex);
            ++lane->stats.rejected_principal;
            return {false, CallAdmissionFailure::PrincipalQuota};
        }
    }

    auto item = std::make_shared<TaskItem>();
    item->lane = lane;
    item->principal = principal;
    item->counts_principal = enforce_principal;
    item->deadline = std::chrono::steady_clock::now() + observer_timeout;
    item->work = std::move(work);
    item->on_timeout = std::move(on_timeout);

    std::optional<CallAdmissionFailure> reject_reason;
    {
        std::lock_guard<std::mutex> lock(lane->mutex);
        if (lane->stopping) {
            reject_reason = CallAdmissionFailure::ShuttingDown;
        } else if (lane->queue.size() >= lane->max_queue) {
            ++lane->stats.rejected_global;
            reject_reason = CallAdmissionFailure::GlobalSaturation;
        } else {
            lane->queue.push_back(item);
            lane->stats.queued = lane->queue.size();
            lane->stats.high_water_queued =
                std::max(lane->stats.high_water_queued, lane->stats.queued);
        }
    }
    if (reject_reason) {
        if (enforce_principal) {
            quota_->release(principal);
        }
        return {false, *reject_reason};
    }
    lane->cv.notify_one();

    if (item->on_timeout) {
        std::lock_guard<std::mutex> tlock(timer_mutex_);
        pending_timers_.push_back(item);
        timer_cv_.notify_one();
    }
    return {true, CallAdmissionFailure::GlobalSaturation};
}

void ServiceCallExecutor::worker_loop(
    const std::shared_ptr<Lane>& lane, const std::shared_ptr<QuotaState>& quota) {
    for (;;) {
        std::shared_ptr<TaskItem> item;
        {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->cv.wait(lock, [&] { return lane->stopping || !lane->queue.empty(); });
            if (lane->queue.empty()) {
                if (lane->stopping) {
                    break;
                }
                continue;
            }
            item = std::move(lane->queue.front());
            lane->queue.pop_front();
            lane->stats.queued = lane->queue.size();
            ++lane->active;
            lane->stats.active = lane->active;
            lane->stats.high_water_active =
                std::max(lane->stats.high_water_active, lane->stats.active);
        }

        bool should_run = true;
        std::function<void()> immediate_timeout_cb;
        {
            std::lock_guard<std::mutex> slock(item->state_mutex);
            if (item->timed_out || item->cancelled_before_start ||
                std::chrono::steady_clock::now() >= item->deadline) {
                should_run = false;
                if (!item->response_claimed) {
                    item->timed_out = true;
                    item->response_claimed = true;
                    immediate_timeout_cb = std::move(item->on_timeout);
                }
                item->completed = true;
            } else {
                item->started = true;
            }
        }

        if (should_run && item->work) {
            auto try_claim = [item]() -> bool {
                std::lock_guard<std::mutex> slock(item->state_mutex);
                if (item->timed_out || item->response_claimed) {
                    return false;
                }
                item->response_claimed = true;
                return true;
            };
            try {
                item->work(try_claim);
            } catch (...) {
                // Worker must never crash process on uncaught exception.
            }
            bool was_late = false;
            {
                std::lock_guard<std::mutex> slock(item->state_mutex);
                item->completed = true;
                was_late = item->timed_out;
            }
            if (was_late) {
                std::lock_guard<std::mutex> llock(lane->mutex);
                ++lane->stats.late_completed;
            }
        } else if (immediate_timeout_cb) {
            {
                std::lock_guard<std::mutex> llock(lane->mutex);
                ++lane->stats.timed_out;
            }
            try {
                immediate_timeout_cb();
            } catch (...) {
            }
        }

        if (item->counts_principal) {
            quota->release(item->principal);
        }

        {
            std::lock_guard<std::mutex> lock(lane->mutex);
            --lane->active;
            lane->stats.active = lane->active;
            if (lane->active == 0 && lane->queue.empty()) {
                lane->idle_cv.notify_all();
            }
        }
    }
}

void ServiceCallExecutor::timer_loop() {
    std::unique_lock<std::mutex> lock(timer_mutex_);
    for (;;) {
        if (stopping_.load(std::memory_order_acquire)) {
            pending_timers_.clear();
            break;
        }
        auto next_wake = std::chrono::steady_clock::now() + std::chrono::milliseconds{25};
        for (const auto& item : pending_timers_) {
            if (item->deadline < next_wake) {
                next_wake = item->deadline;
            }
        }
        if (pending_timers_.empty()) {
            timer_cv_.wait(lock, [&] {
                return stopping_.load(std::memory_order_acquire) || !pending_timers_.empty();
            });
        } else {
            timer_cv_.wait_until(lock, next_wake);
        }
        if (stopping_.load(std::memory_order_acquire)) {
            pending_timers_.clear();
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<std::shared_ptr<Lane>, std::function<void()>>> expired_callbacks;
        for (auto it = pending_timers_.begin(); it != pending_timers_.end();) {
            auto& item = *it;
            bool erase = false;
            {
                std::lock_guard<std::mutex> slock(item->state_mutex);
                if (item->completed || item->response_claimed) {
                    erase = true;
                } else if (now >= item->deadline) {
                    item->timed_out = true;
                    item->response_claimed = true;
                    if (item->on_timeout) {
                        expired_callbacks.emplace_back(item->lane, std::move(item->on_timeout));
                    }
                    erase = true;
                }
            }
            if (erase) {
                it = pending_timers_.erase(it);
            } else {
                ++it;
            }
        }
        lock.unlock();
        for (auto& entry : expired_callbacks) {
            if (entry.first) {
                std::lock_guard<std::mutex> llock(entry.first->mutex);
                ++entry.first->stats.timed_out;
            }
            try {
                entry.second();
            } catch (...) {
            }
        }
        lock.lock();
    }
}

Result<void> ServiceCallExecutor::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (stopped_) {
        return {};
    }
    stopping_.store(true, std::memory_order_release);
    std::shared_ptr<Lane> lanes[5] = {
        control_ordinary_, control_reserve_, query_ordinary_, query_reserve_, maintenance_};
    for (auto& lane : lanes) {
        if (!lane) {
            continue;
        }
        std::vector<std::function<void()>> cancelled_cbs;
        std::vector<std::string> cancelled_principals;
        {
            std::lock_guard<std::mutex> lock(lane->mutex);
            lane->stopping = true;
            while (!lane->queue.empty()) {
                auto item = std::move(lane->queue.front());
                lane->queue.pop_front();
                {
                    std::lock_guard<std::mutex> slock(item->state_mutex);
                    item->cancelled_before_start = true;
                    item->completed = true;
                    if (!item->response_claimed && item->on_timeout) {
                        item->response_claimed = true;
                        cancelled_cbs.push_back(std::move(item->on_timeout));
                    }
                }
                if (item->counts_principal) {
                    cancelled_principals.push_back(item->principal);
                }
            }
            lane->stats.queued = 0;
            lane->cv.notify_all();
        }
        for (const auto& p : cancelled_principals) {
            quota_->release(p);
        }
        for (auto& cb : cancelled_cbs) {
            try {
                cb();
            } catch (...) {
            }
        }
    }
    timer_cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }

    bool all_settled = true;
    for (auto& lane : lanes) {
        if (!lane) {
            continue;
        }
        std::unique_lock<std::mutex> lock(lane->mutex);
        if (!lane->idle_cv.wait_until(lock, deadline, [&] { return lane->active == 0; })) {
            all_settled = false;
        }
    }

    if (all_settled) {
        for (auto& lane : lanes) {
            if (!lane) {
                continue;
            }
            for (auto& w : lane->workers) {
                if (w.joinable()) {
                    w.join();
                }
            }
        }
        stopped_ = true;
        return {};
    }
    // Retain shared_ptr<Lane> and shared_ptr<QuotaState> ownership on detached
    // workers if a hung Core call exceeded shutdown deadline.
    for (auto& lane : lanes) {
        if (!lane) {
            continue;
        }
        for (auto& w : lane->workers) {
            if (w.joinable()) {
                w.detach();
            }
        }
    }
    stopped_ = true;
    return Status(StatusCode::DeadlineExceeded, "active Core call exceeded shutdown deadline");
}

ExecutorMetrics ServiceCallExecutor::metrics() const {
    ExecutorMetrics out;
    auto copy_lane = [](const Lane& lane) {
        std::lock_guard<std::mutex> lock(lane.mutex);
        return lane.stats;
    };
    out.control_ordinary = copy_lane(*control_ordinary_);
    out.control_reserve = copy_lane(*control_reserve_);
    out.query_ordinary = copy_lane(*query_ordinary_);
    out.query_reserve = copy_lane(*query_reserve_);
    out.maintenance = copy_lane(*maintenance_);
    return out;
}

}  // namespace rodaq::server
