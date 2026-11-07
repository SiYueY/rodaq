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
        flushes_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    Result<void> shutdown() override {
        shutdown_.store(true, std::memory_order_release);
        return {};
    }

    Result<void> set_level(LogLevel) override { return {}; }
    Result<void> enable_backtrace(std::size_t) override { return {}; }
    Result<void> disable_backtrace() override { return {}; }
    Result<void> dump_backtrace() override {
        dumps_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    std::vector<LogRecord> records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

    int flushes() const { return flushes_.load(std::memory_order_relaxed); }
    int dumps() const { return dumps_.load(std::memory_order_relaxed); }
    bool shutdown_complete() const { return shutdown_.load(std::memory_order_acquire); }

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
    std::atomic<int> flushes_{0};
    std::atomic<int> dumps_{0};
    std::atomic<bool> shutdown_{false};
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
    Result<void> set_level(LogLevel) override { return {}; }
    Result<void> enable_backtrace(std::size_t) override { return {}; }
    Result<void> disable_backtrace() override { return {}; }
    Result<void> dump_backtrace() override { return {}; }

    void release() { released_.store(true, std::memory_order_release); }
    bool entered() const { return entered_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> entered_{false};
    std::atomic<bool> released_{false};
};

class FirstWriteBlockingBackend final : public LoggingBackend {
public:
    Result<void> write(const LogRecord&) override {
        const bool previous = entered_.exchange(true, std::memory_order_acq_rel);
        if (!previous) {
            while (!released_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        written_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    Result<void> flush() override { return {}; }
    Result<void> shutdown() override { return {}; }
    Result<void> set_level(LogLevel) override { return {}; }
    Result<void> enable_backtrace(std::size_t) override { return {}; }
    Result<void> disable_backtrace() override { return {}; }
    Result<void> dump_backtrace() override { return {}; }

    void release() { released_.store(true, std::memory_order_release); }
    bool entered() const { return entered_.load(std::memory_order_acquire); }
    int written() const { return written_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> entered_{false};
    std::atomic<bool> released_{false};
    std::atomic<int> written_{0};
};

LoggingConfig async_config() {
    LoggingConfig config;
    config.level = LogLevel::Trace;
    config.async.queue_records = 4;
    config.async.queue_bytes = 128;
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
        context.value()->log(LogLevel::Info, category, {}, message);
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
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "first");
    while (!backend->entered()) {
        std::this_thread::yield();
    }
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "second");
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "third");
    context.value()->log(LogLevel::Info, "rodaq.test", {}, std::string(200, 'x'));
    backend->release();
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const auto stats = context.value()->stats();
    EXPECT_GT(stats.records_dropped, 0U);
    EXPECT_GT(stats.bytes_dropped, 0U);
    EXPECT_LE(stats.queue_records_high_water, config.async.queue_records);
    EXPECT_LE(stats.queue_bytes_high_water, config.async.queue_bytes);
}

TEST(LoggingContextTest, OverrunOldestEvictsOnlyQueuedRecords) {
    auto backend = std::make_shared<BlockingBackend>();
    auto config = async_config();
    config.async.queue_records = 2;
    config.async.queue_bytes = 256;
    config.async.overflow_policy = OverflowPolicy::OverrunOldest;
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());

    context.value()->log(LogLevel::Info, "rodaq.test", {}, "first");
    while (!backend->entered()) {
        std::this_thread::yield();
    }
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "second");
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "third");
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "fourth");
    backend->release();

    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const auto stats = context.value()->stats();
    EXPECT_EQ(4U, stats.records_submitted);
    EXPECT_EQ(3U, stats.records_written);
    EXPECT_EQ(1U, stats.records_dropped);
    EXPECT_EQ(1U, stats.overrun_count);
    EXPECT_EQ(0U, stats.discard_count);
}

TEST(LoggingContextTest, BlockPolicyWaitsForCapacityAndReleasesOnShutdown) {
    auto backend = std::make_shared<FirstWriteBlockingBackend>();
    auto config = async_config();
    config.async.queue_records = 1;
    config.async.queue_bytes = 128;
    config.async.overflow_policy = OverflowPolicy::Block;
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());

    context.value()->log(LogLevel::Info, "rodaq.test", {}, "first");
    while (!backend->entered()) {
        std::this_thread::yield();
    }
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "second");

    std::atomic<bool> producer_returned{false};
    std::thread producer([context = context.value().get(), &producer_returned] {
        context->log(LogLevel::Info, "rodaq.test", {}, "third");
        producer_returned.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(producer_returned.load(std::memory_order_acquire));
    backend->release();
    producer.join();

    EXPECT_TRUE(producer_returned.load(std::memory_order_acquire));
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(3, backend->written());
}

TEST(LoggingContextTest, PeriodicFlushRunsWhileTheQueueIsEmpty) {
    auto backend = std::make_shared<RecordingBackend>();
    auto config = async_config();
    config.flush.interval = std::chrono::milliseconds(5);
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (backend->flushes() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(backend->flushes(), 0);
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggingContextTest, FlushRequestPreservesRecordOrdering) {
    auto backend = std::make_shared<RecordingBackend>();
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    auto logger = context.value()->make_logger("rodaq.test");
    ASSERT_TRUE(logger.ok());

    logger.value().write(LogLevel::Info, {}, "before flush");
    ASSERT_TRUE(logger.value().request_flush().ok());
    ASSERT_TRUE(context.value()->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));

    const auto records = backend->records();
    ASSERT_EQ(1U, records.size());
    EXPECT_EQ("before flush", records[0].message);
    EXPECT_GE(backend->flushes(), 1);
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggingContextTest, BacktraceControlsAndDumpAreQueuedInOrder) {
    auto backend = std::make_shared<RecordingBackend>();
    LoggingConfig config;
    config.level = LogLevel::Error;
    config.async.queue_records = 8;
    config.async.queue_bytes = 1024;
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());
    auto logger = context.value()->make_logger("rodaq.test");
    ASSERT_TRUE(logger.ok());

    ASSERT_TRUE(logger.value().enable_backtrace(8).ok());
    EXPECT_TRUE(logger.value().should_log(LogLevel::Debug));
    ASSERT_TRUE(logger.value().dump_backtrace().ok());
    ASSERT_TRUE(context.value()->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(1, backend->dumps());

    ASSERT_TRUE(logger.value().disable_backtrace().ok());
    EXPECT_FALSE(logger.value().should_log(LogLevel::Debug));
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggingContextTest, DetectsDirectReentrantLogging) {
    class ReentrantBackend final : public LoggingBackend {
    public:
        explicit ReentrantBackend(LoggingContext** context) : context_(context) {}
        Result<void> write(const LogRecord&) override {
            (*context_)->log(LogLevel::Info, "rodaq.test", {}, "recursive");
            return {};
        }
        Result<void> flush() override { return {}; }
        Result<void> shutdown() override { return {}; }
        Result<void> set_level(LogLevel) override { return {}; }
        Result<void> enable_backtrace(std::size_t) override { return {}; }
        Result<void> disable_backtrace() override { return {}; }
        Result<void> dump_backtrace() override { return {}; }

    private:
        LoggingContext** context_;
    };

    LoggingContext* raw = nullptr;
    auto backend = std::make_shared<ReentrantBackend>(&raw);
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    raw = context.value().get();
    raw->log(LogLevel::Info, "rodaq.test", {}, "outer");
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
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "blocked");
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
        Result<void> set_level(LogLevel) override { return {}; }
        Result<void> enable_backtrace(std::size_t) override { return {}; }
        Result<void> disable_backtrace() override { return {}; }
        Result<void> dump_backtrace() override { return {}; }
    };

    auto backend = std::make_shared<ThrowingBackend>();
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "throwing backend");
    ASSERT_TRUE(context.value()->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(1U, context.value()->stats().backend_failures);
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggingContextTest, FlushDeadlineReportsTimeoutWithoutDiscardingWorkerState) {
    auto backend = std::make_shared<BlockingBackend>();
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "blocked flush");
    while (!backend->entered()) {
        std::this_thread::yield();
    }

    EXPECT_FALSE(
        context.value()->flush(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
    EXPECT_EQ(1U, context.value()->stats().flush_timeouts);

    backend->release();
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggingContextTest, BackendFlushFailureIsContainedAndObservable) {
    class ThrowingFlushBackend final : public LoggingBackend {
    public:
        Result<void> write(const LogRecord&) override { return {}; }
        Result<void> flush() override { throw std::runtime_error("flush failed"); }
        Result<void> shutdown() override { return {}; }
        Result<void> set_level(LogLevel) override { return {}; }
        Result<void> enable_backtrace(std::size_t) override { return {}; }
        Result<void> disable_backtrace() override { return {}; }
        Result<void> dump_backtrace() override { return {}; }
    };

    auto backend = std::make_shared<ThrowingFlushBackend>();
    auto context = LoggingContext::create(async_config(), backend);
    ASSERT_TRUE(context.ok());
    context.value()->log(LogLevel::Info, "rodaq.test", {}, "record");
    EXPECT_FALSE(
        context.value()->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_EQ(1U, context.value()->stats().backend_failures);
    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

}  // namespace
}  // namespace rodaq
