#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rodaq::plugin {

class LifecycleExecutor final {
public:
    explicit LifecycleExecutor(std::size_t worker_count = 2u, std::size_t queue_capacity = 64u);
    LifecycleExecutor(const LifecycleExecutor&) = delete;
    LifecycleExecutor& operator=(const LifecycleExecutor&) = delete;
    ~LifecycleExecutor();

    [[nodiscard]] bool submit(std::function<void()> task);
    void stop_and_join() noexcept;

private:
    void worker_loop();

    std::size_t queue_capacity_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    bool joined_ = false;
};

}  // namespace rodaq::plugin
