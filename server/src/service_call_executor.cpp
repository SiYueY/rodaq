#include "service_call_executor.hpp"

#include <algorithm>
#include <utility>

namespace rodaq::server {

struct ServiceCallExecutor::CallState {
    RequestContext context;
    std::function<void(const std::function<bool()>&)> work;
    TimeoutFn on_timeout;
    std::mutex mutex;
    ServiceCallLifecycle state{ServiceCallLifecycle::Queued};
    bool response_claimed{false};

    bool try_claim_response() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (response_claimed || state == ServiceCallLifecycle::TimedOutButInFlight) {
            return false;
        }
        response_claimed = true;
        return true;
    }
};

struct ServiceCallExecutor::Lane {
    std::uint32_t max_queue{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<CallState>> queue;
    std::vector<std::thread> workers;
    bool stopping{false};
    std::atomic<std::uint64_t> active{0};
    std::atomic<std::uint64_t> hwm{0};
    std::atomic<std::uint64_t>* late_counter{nullptr};

    Lane(std::uint32_t worker_count, std::uint32_t queue_cap, std::atomic<std::uint64_t>* late)
    : max_queue(queue_cap), late_counter(late) {
        workers.reserve(worker_count);
        for (std::uint32_t i = 0; i < worker_count; ++i) {
            workers.emplace_back([this]() { worker_loop(); });
        }
    }

    ~Lane() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    bool enqueue(std::shared_ptr<CallState> call) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || queue.size() >= max_queue) {
            return false;
        }
        queue.push_back(std::move(call));
        cv.notify_one();
        return true;
    }

    void worker_loop() noexcept {
        for (;;) {
            std::shared_ptr<CallState> call;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [this]() { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    return;
                }
                call = std::move(queue.front());
                queue.pop_front();
            }

            {
                std::lock_guard<std::mutex> lock(call->mutex);
                if (call->state == ServiceCallLifecycle::TimedOutButInFlight ||
                    call->response_claimed) {
                    // Timed out while queued before Core call started: safe to skip.
                    call->state = ServiceCallLifecycle::LateCompleted;
                    continue;
                }
                call->state = ServiceCallLifecycle::Running;
            }

            const auto cur = active.fetch_add(1, std::memory_order_relaxed) + 1;
            auto prev_hwm = hwm.load(std::memory_order_relaxed);
            while (cur > prev_hwm &&
                   !hwm.compare_exchange_weak(prev_hwm, cur, std::memory_order_relaxed)) {
            }

            try {
                call->work([call]() -> bool { return call->try_claim_response(); });
            } catch (...) {
                // Swallow to keep worker thread alive; response_claimed checked below.
            }

            {
                std::lock_guard<std::mutex> lock(call->mutex);
                if (call->state == ServiceCallLifecycle::TimedOutButInFlight) {
                    call->state = ServiceCallLifecycle::LateCompleted;
                    if (late_counter) {
                        late_counter->fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    call->state = ServiceCallLifecycle::Completed;
                }
            }
            active.fetch_sub(1, std::memory_order_relaxed);
        }
    }
};

Result<std::unique_ptr<ServiceCallExecutor>> ServiceCallExecutor::create(
    const ExecutionConfig& config) {
    try {
        auto exec = std::unique_ptr<ServiceCallExecutor>(new ServiceCallExecutor());
        exec->control_ordinary_ = std::make_unique<Lane>(
            config.control_workers, config.control_queue, &exec->late_completed_);
        exec->control_reserve_ = std::make_unique<Lane>(
            config.control_reserve_workers, config.control_reserve_queue, &exec->late_completed_);
        exec->query_ordinary_ = std::make_unique<Lane>(
            config.query_workers, config.query_queue, &exec->late_completed_);
        exec->query_reserve_ = std::make_unique<Lane>(
            config.query_reserve_workers, config.query_reserve_queue, &exec->late_completed_);
        exec->maintenance_ = std::make_unique<Lane>(
            config.maintenance_workers, config.maintenance_queue, &exec->late_completed_);
        exec->timer_thread_ = std::thread([raw = exec.get()]() { raw->timer_loop(); });
        return exec;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "Failed to allocate ServiceCallExecutor");
    }
}

ServiceCallExecutor::~ServiceCallExecutor() noexcept {
    if (!stopped_) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

void ServiceCallExecutor::timer_loop() noexcept {
    using namespace std::chrono_literals;
    std::unique_lock<std::mutex> lock(timer_mutex_);
    while (!stopping_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<TimeoutFn> expired_callbacks;
        auto next_wake = now + 50ms;

        for (auto it = watched_.begin(); it != watched_.end();) {
            auto& call = *it;
            bool remove = false;
            TimeoutFn cb;
            {
                std::lock_guard<std::mutex> clock(call->mutex);
                if (call->state == ServiceCallLifecycle::Completed ||
                    call->state == ServiceCallLifecycle::LateCompleted || call->response_claimed) {
                    remove = true;
                } else if (now >= call->context.observer_deadline) {
                    call->state = ServiceCallLifecycle::TimedOutButInFlight;
                    call->response_claimed = true;
                    cb = std::move(call->on_timeout);
                    timed_out_in_flight_.fetch_add(1, std::memory_order_relaxed);
                    remove = true;
                } else if (call->context.observer_deadline < next_wake) {
                    next_wake = call->context.observer_deadline;
                }
            }
            if (cb) {
                expired_callbacks.push_back(std::move(cb));
            }
            if (remove) {
                it = watched_.erase(it);
            } else {
                ++it;
            }
        }

        lock.unlock();
        for (auto& cb : expired_callbacks) {
            try {
                cb();
            } catch (...) {
            }
        }
        lock.lock();
        if (stopping_.load(std::memory_order_acquire)) {
            break;
        }
        timer_cv_.wait_until(lock, next_wake);
    }
}

Result<void> ServiceCallExecutor::submit(
    CallDomain domain, AdmissionClass admission, RequestContext context,
    std::function<void(const std::function<bool()>& claim_response)> work, TimeoutFn on_timeout) {
    if (stopping_.load(std::memory_order_acquire)) {
        return Status(StatusCode::Unavailable, "Server is draining");
    }

    Lane* target = nullptr;
    switch (domain) {
        case CallDomain::Control:
            target = (admission == AdmissionClass::Reserve) ? control_reserve_.get()
                                                            : control_ordinary_.get();
            break;
        case CallDomain::Query:
            target = (admission == AdmissionClass::Reserve) ? query_reserve_.get()
                                                            : query_ordinary_.get();
            break;
        case CallDomain::Maintenance:
            target = maintenance_.get();
            break;
    }

    auto call = std::make_shared<CallState>();
    call->context = std::move(context);
    call->work = std::move(work);
    call->on_timeout = std::move(on_timeout);

    if (!target->enqueue(call)) {
        rejected_saturation_.fetch_add(1, std::memory_order_relaxed);
        return Status(StatusCode::Unavailable, "Execution domain queue saturated");
    }

    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        watched_.push_back(std::move(call));
    }
    timer_cv_.notify_one();
    return {};
}

Result<void> ServiceCallExecutor::shutdown(std::chrono::steady_clock::time_point /*deadline*/) {
    if (stopped_) {
        return {};
    }
    stopping_.store(true, std::memory_order_release);
    timer_cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
    control_ordinary_.reset();
    control_reserve_.reset();
    query_ordinary_.reset();
    query_reserve_.reset();
    maintenance_.reset();
    stopped_ = true;
    return {};
}

ExecutorMetrics ServiceCallExecutor::metrics() const noexcept {
    ExecutorMetrics m;
    if (control_ordinary_) {
        m.control_active =
            control_ordinary_->active.load(std::memory_order_relaxed) +
            (control_reserve_ ? control_reserve_->active.load(std::memory_order_relaxed) : 0);
        m.control_hwm = control_ordinary_->hwm.load(std::memory_order_relaxed);
    }
    if (query_ordinary_) {
        m.query_active =
            query_ordinary_->active.load(std::memory_order_relaxed) +
            (query_reserve_ ? query_reserve_->active.load(std::memory_order_relaxed) : 0);
        m.query_hwm = query_ordinary_->hwm.load(std::memory_order_relaxed);
    }
    m.rejected_saturation = rejected_saturation_.load(std::memory_order_relaxed);
    m.timed_out_in_flight = timed_out_in_flight_.load(std::memory_order_relaxed);
    m.late_completed = late_completed_.load(std::memory_order_relaxed);
    return m;
}

}  // namespace rodaq::server
