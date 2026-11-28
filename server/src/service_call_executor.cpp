#include "service_call_executor.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace rodaq::server {

struct ControlExecutor::TaskItem {
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

struct ControlExecutor::SharedState {
    std::uint32_t worker_count{2};
    std::uint32_t max_queue{64};
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable idle_cv;
    std::deque<std::shared_ptr<TaskItem>> queue;
    std::uint64_t active{0};
    ControlMetrics stats;
    bool stopping{false};
    std::vector<std::thread> workers;
};

ControlExecutor::ControlExecutor(ExecutionConfig config)
: config_(config), state_(std::make_shared<SharedState>()) {}

ControlExecutor::~ControlExecutor() noexcept {
    if (!stopped_) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

Result<std::unique_ptr<ControlExecutor>> ControlExecutor::create(const ExecutionConfig& config) {
    try {
        std::unique_ptr<ControlExecutor> exec(new ControlExecutor(config));
        exec->state_->worker_count = config.control_workers;
        exec->state_->max_queue = config.control_queue;
        exec->state_->workers.reserve(config.control_workers);
        for (std::uint32_t i = 0; i < config.control_workers; ++i) {
            exec->state_->workers.emplace_back([st = exec->state_] { worker_loop(st); });
        }
        exec->timer_thread_ = std::thread([raw = exec.get()] { raw->timer_loop(); });
        return exec;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "failed to allocate ControlExecutor");
    } catch (const std::system_error&) {
        return Status(StatusCode::ResourceExhausted, "failed to start ControlExecutor threads");
    }
}

ExecutorSubmitResult ControlExecutor::submit(
    std::chrono::nanoseconds observer_timeout,
    std::function<void(const std::function<bool()>& try_claim_response)> work,
    std::function<void()> on_timeout) {
    if (stopping_.load(std::memory_order_acquire)) {
        return {false, CallAdmissionFailure::ShuttingDown};
    }

    std::shared_ptr<TaskItem> item;
    try {
        item = std::make_shared<TaskItem>();
        item->deadline = std::chrono::steady_clock::now() + observer_timeout;
        item->work = std::move(work);
        item->on_timeout = std::move(on_timeout);

        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->stopping) {
                return {false, CallAdmissionFailure::ShuttingDown};
            }
            if (state_->queue.size() >= state_->max_queue) {
                ++state_->stats.rejected;
                return {false, CallAdmissionFailure::GlobalSaturation};
            }
            state_->queue.push_back(item);
            state_->stats.queued = state_->queue.size();
            state_->stats.high_water_queued =
                std::max(state_->stats.high_water_queued, state_->stats.queued);
        }
        state_->cv.notify_one();

        if (item->on_timeout) {
            std::lock_guard<std::mutex> tlock(timer_mutex_);
            pending_timers_.push_back(item);
            timer_cv_.notify_one();
        }
        return {true, CallAdmissionFailure::GlobalSaturation};
    } catch (const std::bad_alloc&) {
        return {false, CallAdmissionFailure::GlobalSaturation};
    }
}

void ControlExecutor::worker_loop(const std::shared_ptr<SharedState>& state) {
    for (;;) {
        std::shared_ptr<TaskItem> item;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
            if (state->queue.empty()) {
                if (state->stopping) {
                    break;
                }
                continue;
            }
            item = std::move(state->queue.front());
            state->queue.pop_front();
            state->stats.queued = state->queue.size();
            ++state->active;
            state->stats.active = state->active;
            state->stats.high_water_active =
                std::max(state->stats.high_water_active, state->stats.active);
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
            }
            bool was_late = false;
            {
                std::lock_guard<std::mutex> slock(item->state_mutex);
                item->completed = true;
                was_late = item->timed_out;
            }
            if (was_late) {
                std::lock_guard<std::mutex> llock(state->mutex);
                ++state->stats.late_completed;
            }
        } else if (immediate_timeout_cb) {
            {
                std::lock_guard<std::mutex> llock(state->mutex);
                ++state->stats.timed_out;
            }
            try {
                immediate_timeout_cb();
            } catch (...) {
            }
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            --state->active;
            state->stats.active = state->active;
            if (state->active == 0 && state->queue.empty()) {
                state->idle_cv.notify_all();
            }
        }
    }
}

void ControlExecutor::timer_loop() {
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
        std::vector<std::function<void()>> expired_callbacks;
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
                        expired_callbacks.push_back(std::move(item->on_timeout));
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
        for (auto& cb : expired_callbacks) {
            {
                std::lock_guard<std::mutex> llock(state_->mutex);
                ++state_->stats.timed_out;
            }
            try {
                cb();
            } catch (...) {
            }
        }
        lock.lock();
    }
}

Result<void> ControlExecutor::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (stopped_) {
        return {};
    }
    stopping_.store(true, std::memory_order_release);
    std::vector<std::function<void()>> cancelled_cbs;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        while (!state_->queue.empty()) {
            auto item = std::move(state_->queue.front());
            state_->queue.pop_front();
            std::lock_guard<std::mutex> slock(item->state_mutex);
            item->cancelled_before_start = true;
            item->completed = true;
            if (!item->response_claimed && item->on_timeout) {
                item->response_claimed = true;
                cancelled_cbs.push_back(std::move(item->on_timeout));
            }
        }
        state_->stats.queued = 0;
        state_->cv.notify_all();
    }
    for (auto& cb : cancelled_cbs) {
        try {
            cb();
        } catch (...) {
        }
    }
    timer_cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }

    bool settled = false;
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        settled = state_->idle_cv.wait_until(lock, deadline, [&] { return state_->active == 0; });
    }
    if (settled) {
        for (auto& w : state_->workers) {
            if (w.joinable()) {
                w.join();
            }
        }
        stopped_ = true;
        return {};
    }
    for (auto& w : state_->workers) {
        if (w.joinable()) {
            w.detach();
        }
    }
    stopped_ = true;
    return Status(StatusCode::DeadlineExceeded, "active Core call exceeded shutdown deadline");
}

ControlMetrics ControlExecutor::metrics() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
}

}  // namespace rodaq::server
