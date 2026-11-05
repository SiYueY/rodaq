#include "runtime/logging_context.hpp"
#include "runtime/quarantine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rodaq::runtime {
namespace {

class RecordingBackend final : public LoggingBackend {
public:
    Result<void> Write(const LogRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(record);
        return {};
    }

    Result<void> Flush() override {
        ++flushes_;
        return {};
    }

    Result<void> Shutdown() override {
        shutdown_ = true;
        return {};
    }

    std::vector<LogRecord> Records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    int Flushes() const { return flushes_; }
    bool ShutdownComplete() const { return shutdown_; }

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
    int flushes_ = 0;
    bool shutdown_ = false;
};

class BlockingBackend final : public LoggingBackend {
public:
    Result<void> Write(const LogRecord&) override {
        entered_.store(true, std::memory_order_release);
        while (!released_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return {};
    }

    Result<void> Flush() override { return {}; }
    Result<void> Shutdown() override { return {}; }

    void Release() { released_.store(true, std::memory_order_release); }
    bool Entered() const { return entered_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> entered_{false};
    std::atomic<bool> released_{false};
};

LoggingConfig async_config() {
    LoggingConfig config;
    config.level = RODAQ_LOG_TRACE;
    config.queue_records = 4;
    config.queue_bytes = 128;
    config.flush_timeout = std::chrono::seconds(1);
    config.shutdown_timeout = std::chrono::seconds(1);
    return config;
}

TEST(LoggingContextTest, OwnsCopiedCategoryAndMessageForAsyncDelivery) {
    auto backend = std::make_shared<RecordingBackend>();
    auto context = LoggingContext::Create(async_config(), backend);
    ASSERT_TRUE(context.has_value());
    {
        std::string category = "rodaq.test";
        std::string message = "message";
        context.value()->Log(RODAQ_LOG_INFO, category, message);
    }
    ASSERT_TRUE(context.value()->Flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const auto records = backend->Records();
    ASSERT_EQ(1U, records.size());
    EXPECT_EQ("rodaq.test", records[0].category);
    EXPECT_EQ("message", records[0].message);
    EXPECT_TRUE(
        context.value()->Shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_TRUE(backend->ShutdownComplete());
}

TEST(LoggingContextTest, DropsNewRecordsAtRecordAndByteCapacity) {
    auto backend = std::make_shared<BlockingBackend>();
    auto config = async_config();
    auto context = LoggingContext::Create(config, backend);
    ASSERT_TRUE(context.has_value());
    context.value()->Log(RODAQ_LOG_INFO, "rodaq.test", "first");
    while (!backend->Entered()) {
        std::this_thread::yield();
    }
    context.value()->Log(RODAQ_LOG_INFO, "rodaq.test", "second");
    context.value()->Log(RODAQ_LOG_INFO, "rodaq.test", "third");
    context.value()->Log(RODAQ_LOG_INFO, "rodaq.test", std::string(200, 'x'));
    backend->Release();
    ASSERT_TRUE(
        context.value()->Shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const auto stats = context.value()->Stats();
    EXPECT_GT(stats.records_dropped, 0U);
    EXPECT_GT(stats.bytes_dropped, 0U);
    EXPECT_LE(stats.queue_records_high_water, config.queue_records);
    EXPECT_LE(stats.queue_bytes_high_water, config.queue_bytes);
}

TEST(LoggingContextTest, DetectsDirectReentrantLogging) {
    class ReentrantBackend final : public LoggingBackend {
    public:
        explicit ReentrantBackend(LoggingContext** context) : context_(context) {}
        Result<void> Write(const LogRecord&) override {
            (*context_)->Log(RODAQ_LOG_INFO, "rodaq.test", "recursive");
            return {};
        }
        Result<void> Flush() override { return {}; }
        Result<void> Shutdown() override { return {}; }

    private:
        LoggingContext** context_;
    };

    LoggingContext* raw = nullptr;
    auto backend = std::make_shared<ReentrantBackend>(&raw);
    auto context = LoggingContext::Create(async_config(), backend);
    ASSERT_TRUE(context.has_value());
    raw = context.value().get();
    raw->Log(RODAQ_LOG_INFO, "rodaq.test", "outer");
    ASSERT_TRUE(raw->Flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(1U, raw->Stats().reentrant_drops);
    ASSERT_TRUE(raw->Shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggingContextTest, QuarantinesWorkerWhenShutdownTimesOut) {
    auto& manager = QuarantineManager::Process();
    manager.ReapCompleted();
    const auto before = manager.RetainedWorkerCount();
    auto backend = std::make_shared<BlockingBackend>();
    struct ReleaseGuard {
        std::shared_ptr<BlockingBackend> backend;
        ~ReleaseGuard() { backend->Release(); }
    } release{backend};
    auto context = LoggingContext::Create(async_config(), backend);
    ASSERT_TRUE(context.has_value());
    context.value()->Log(RODAQ_LOG_INFO, "rodaq.test", "blocked");
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!backend->Entered() && std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(backend->Entered());
    EXPECT_FALSE(
        context.value()->Shutdown(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
    context.value()->QuarantineWorker("test logging worker");
    EXPECT_EQ(manager.RetainedWorkerCount(), before + 1U);
    backend->Release();
    context.value().reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (manager.RetainedWorkerCount() != before && std::chrono::steady_clock::now() < deadline) {
        manager.ReapCompleted();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(manager.RetainedWorkerCount(), before);
}

TEST(LoggingContextTest, ContainsThrowingBackendWithoutTerminating) {
    class ThrowingBackend final : public LoggingBackend {
    public:
        Result<void> Write(const LogRecord&) override {
            throw std::runtime_error("backend write failure");
        }
        Result<void> Flush() override { return {}; }
        Result<void> Shutdown() override { return {}; }
    };

    auto backend = std::make_shared<ThrowingBackend>();
    auto context = LoggingContext::Create(async_config(), backend);
    ASSERT_TRUE(context.has_value());
    context.value()->Log(RODAQ_LOG_INFO, "rodaq.test", "throwing backend");
    ASSERT_TRUE(context.value()->Flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(1U, context.value()->Stats().backend_failures);
    ASSERT_TRUE(
        context.value()->Shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

}  // namespace
}  // namespace rodaq::runtime
