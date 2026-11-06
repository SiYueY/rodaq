#include "logging/logging_context.hpp"
#include "plugin/plugin_quarantine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rodaq {
namespace {

class RecordingBackend final : public LoggingBackend {
public:
    Result<void> write(const LogRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(record);
        return {};
    }

    Result<void> flush() override {
        ++flushes_;
        return {};
    }

    Result<void> shutdown() override {
        shutdown_ = true;
        return {};
    }

    std::vector<LogRecord> records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    int flushes() const { return flushes_; }
    bool shutdown_complete() const { return shutdown_; }

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
    int flushes_ = 0;
    bool shutdown_ = false;
};

class BlockingBackend final : public LoggingBackend {
public:
    Result<void> write(const LogRecord&) override {
        entered_.store(true, std::memory_order_release);
        while (!released_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {};
    }

    Result<void> flush() override { return {}; }
    Result<void> shutdown() override { return {}; }

    void release() { released_.store(true, std::memory_order_release); }
    bool entered() const { return entered_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> entered_{false};
    std::atomic<bool> released_{false};
};

LoggingConfig async_config() {
    LoggingConfig config;
    config.level = LogLevel::Trace;
    config.queue_records = 4;
    config.queue_bytes = 128;
    config.flush_timeout = std::chrono::seconds(1);
    config.shutdown_timeout = std::chrono::seconds(1);
    return config;
}

TEST(LoggingContextTest, OwnsCopiedCategoryAndMessageForAsyncDelivery) {
    auto backend = std::make_shared<RecordingBackend>();
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    {
        std::string category = "rodaq.test";
        std::string message = "message";
        context.value()->log(LogLevel::Info, category, message);
    }
    ASSERT_TRUE(context.value()->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const auto records = backend->records();
    ASSERT_EQ(1U, records.size());
    EXPECT_EQ("rodaq.test", records[0].category);
    EXPECT_EQ("message", records[0].message);
    EXPECT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_TRUE(backend->shutdown_complete());
}

TEST(LoggingContextTest, DropsNewRecordsAtRecordAndByteCapacity) {
    auto backend = std::make_shared<BlockingBackend>();
    auto config = async_config();
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());
    context.value()->log(LogLevel::Info, "rodaq.test", "first");
    while (!backend->entered()) {
        std::this_thread::yield();
    }
    context.value()->log(LogLevel::Info, "rodaq.test", "second");
    context.value()->log(LogLevel::Info, "rodaq.test", "third");
    context.value()->log(LogLevel::Info, "rodaq.test", std::string(200, 'x'));
    backend->release();
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const auto stats = context.value()->stats();
    EXPECT_GT(stats.records_dropped, 0U);
    EXPECT_GT(stats.bytes_dropped, 0U);
    EXPECT_LE(stats.queue_records_high_water, config.queue_records);
    EXPECT_LE(stats.queue_bytes_high_water, config.queue_bytes);
}

TEST(LoggingContextTest, DetectsDirectReentrantLogging) {
    class ReentrantBackend final : public LoggingBackend {
    public:
        explicit ReentrantBackend(LoggingContext** context) : context_(context) {}
        Result<void> write(const LogRecord&) override {
            (*context_)->log(LogLevel::Info, "rodaq.test", "recursive");
            return {};
        }
        Result<void> flush() override { return {}; }
        Result<void> shutdown() override { return {}; }

    private:
        LoggingContext** context_;
    };

    LoggingContext* raw = nullptr;
    auto backend = std::make_shared<ReentrantBackend>(&raw);
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    raw = context.value().get();
    raw->log(LogLevel::Info, "rodaq.test", "outer");
    ASSERT_TRUE(raw->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(1U, raw->stats().reentrant_drops);
    ASSERT_TRUE(raw->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggingContextTest, QuarantinesWorkerWhenShutdownTimesOut) {
    auto& manager = plugin::PluginQuarantine::process();
    manager.reap_completed();
    const auto before = manager.retained_worker_count();
    auto backend = std::make_shared<BlockingBackend>();
    struct ReleaseGuard {
        std::shared_ptr<BlockingBackend> backend;
        ~ReleaseGuard() { backend->release(); }
    } release{backend};
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    context.value()->log(LogLevel::Info, "rodaq.test", "blocked");
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!backend->entered() && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(backend->entered());
    EXPECT_FALSE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
    context.value()->quarantine_worker("test logging worker");
    EXPECT_EQ(manager.retained_worker_count(), before + 1U);
    backend->release();
    context.value().reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (manager.retained_worker_count() != before &&
           std::chrono::steady_clock::now() < deadline) {
        manager.reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(manager.retained_worker_count(), before);
}

TEST(LoggingContextTest, ContainsThrowingBackendWithoutTerminating) {
    class ThrowingBackend final : public LoggingBackend {
    public:
        Result<void> write(const LogRecord&) override {
            throw std::runtime_error("backend write failure");
        }
        Result<void> flush() override { return {}; }
        Result<void> shutdown() override { return {}; }
    };

    auto backend = std::make_shared<ThrowingBackend>();
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    context.value()->log(LogLevel::Info, "rodaq.test", "throwing backend");
    ASSERT_TRUE(context.value()->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(1U, context.value()->stats().backend_failures);
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

}  // namespace
}  // namespace rodaq
