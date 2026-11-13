#include "lifecycle_executor.hpp"

#include <utility>

namespace rodaq::plugin {

LifecycleExecutor::LifecycleExecutor(std::size_t worker_count, std::size_t queue_capacity)
: queue_capacity_(queue_capacity) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0u; index < worker_count; ++index) {
        workers_.emplace_back(&LifecycleExecutor::worker_loop, this);
    }
}

LifecycleExecutor::~LifecycleExecutor() { stop_and_join(); }

void LifecycleExecutor::stop_and_join() noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (joined_) {
            return;
        }
        stopping_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    std::lock_guard<std::mutex> lock{mutex_};
    joined_ = true;
}

bool LifecycleExecutor::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (stopping_ || tasks_.size() >= queue_capacity_) {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    condition_.notify_one();
    return true;
}

void LifecycleExecutor::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

}  // namespace rodaq::plugin
