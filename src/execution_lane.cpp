#include "execution_lane.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq {

struct ExecutionLane::State {
    mutable std::mutex mutex;
    std::mutex shutdown_mutex;
    std::condition_variable ready;
    std::condition_variable settled;
    std::vector<std::shared_ptr<Work>> queue;
    std::vector<std::thread> workers;
    std::size_t worker_count{0};
    std::size_t head{0};
    std::size_t size{0};
    std::size_t running{0};
    bool accepting{true};
    bool stopping{false};

    void run() noexcept {
        for (;;) {
            std::shared_ptr<Work> work;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return stopping || size != 0; });
                if (stopping) {
                    return;
                }
                work = std::move(queue[head]);
                head = (head + 1) % queue.size();
                --size;
                ++running;
            }
            work->run();
            // Account for the actual frame's destruction outside the queue lock.
            work.reset();
            {
                std::lock_guard<std::mutex> lock(mutex);
                --running;
                if (running == 0 && size == 0) {
                    settled.notify_all();
                }
            }
        }
    }
};

ExecutionLane::ExecutionLane(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

ExecutionLane::~ExecutionLane() noexcept {
    if (!state_->workers.empty()) {
        std::terminate();
    }
}

Result<std::shared_ptr<ExecutionLane>> ExecutionLane::create(
    std::size_t workers, std::size_t capacity) {
    if (workers == 0 || workers > 16 || capacity == 0 || capacity > 4096) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->queue.resize(capacity);
        state->workers.reserve(workers);
        state->worker_count = workers;
        auto lane = std::shared_ptr<ExecutionLane>(new ExecutionLane(std::move(state)));
        auto& owner = *lane->state_;
        try {
            for (std::size_t i = 0; i < workers; ++i) {
                owner.workers.emplace_back([state = &owner] { state->run(); });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(owner.mutex);
                owner.stopping = true;
            }
            owner.ready.notify_all();
            for (auto& worker : owner.workers) {
                worker.join();
            }
            owner.workers.clear();
            throw;
        }
        return lane;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::system_error&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> ExecutionLane::submit(std::shared_ptr<Work> work) {
    if (!work) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->accepting) {
        return Status(StatusCode::InvalidState, {});
    }
    if (state_->size == state_->queue.size()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    state_->queue[(state_->head + state_->size) % state_->queue.size()] = std::move(work);
    ++state_->size;
    state_->ready.notify_one();
    return {};
}

LaneSnapshot ExecutionLane::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return {
        state_->worker_count, state_->queue.size(), state_->size, state_->running,
        state_->accepting};
}

Result<void> ExecutionLane::shutdown(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> shutdown(state_->shutdown_mutex, std::try_to_lock);
    if (!shutdown.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->accepting = false;
        if (!state_->settled.wait_until(
                lock, deadline, [&] { return state_->size == 0 && state_->running == 0; })) {
            return Status(StatusCode::DeadlineExceeded, {});
        }
        state_->stopping = true;
    }
    state_->ready.notify_all();
    for (auto& worker : state_->workers) {
        worker.join();
    }
    state_->workers.clear();
    return {};
}

}  // namespace rodaq
