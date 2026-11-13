#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace rodaq::plugin::reference_test {

template <typename Item>
class BoundedHandoff final {
public:
    using Consumer = std::function<void(Item&&)>;

    BoundedHandoff(std::size_t capacity, Consumer consumer)
    : capacity_(capacity), consumer_(std::move(consumer)) {
        worker_ = std::thread([this] { run(); });
    }

    BoundedHandoff(const BoundedHandoff&) = delete;
    BoundedHandoff& operator=(const BoundedHandoff&) = delete;

    ~BoundedHandoff() { stop(); }

    [[nodiscard]] bool push(Item item) {
        std::lock_guard<std::mutex> lock{mutex_};
        if (stopping_) {
            rejected_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        if (queue_.size() >= capacity_) {
            dropped_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        queue_.push_back(std::move(item));
        condition_.notify_one();
        return true;
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::size_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    void run() noexcept {
        while (true) {
            Item item;
            {
                std::unique_lock<std::mutex> lock{mutex_};
                condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) {
                    return;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            consumer_(std::move(item));
        }
    }

    std::size_t capacity_;
    Consumer consumer_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Item> queue_;
    bool stopping_ = false;
    std::thread worker_;
    std::atomic<std::size_t> dropped_{0u};
    std::atomic<std::size_t> rejected_{0u};
};

}  // namespace rodaq::plugin::reference_test
