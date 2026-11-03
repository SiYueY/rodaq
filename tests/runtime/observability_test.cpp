#include <rodaq/runtime/latency_histogram.hpp>
#include <rodaq/runtime/logger.hpp>
#include <rodaq/runtime/runtime_stats.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace rodaq {
namespace {

TEST(RuntimeStatsTest, RecordsCountersAndSnapshot) {
    RuntimeStats stats;
    stats.RecordAccepted(10);
    stats.RecordAccepted(20);
    stats.RecordRejected();
    stats.RecordTimedOut();
    stats.RecordCancelled();
    stats.RecordGateClosed();

    const RuntimeStatsSnapshot snapshot = stats.Snapshot();
    EXPECT_EQ(snapshot.accepted_records, 2U);
    EXPECT_EQ(snapshot.accepted_bytes, 30U);
    EXPECT_EQ(snapshot.rejected_records, 1U);
    EXPECT_EQ(snapshot.timed_out_records, 1U);
    EXPECT_EQ(snapshot.cancelled_records, 1U);
    EXPECT_EQ(snapshot.gate_closed_records, 1U);
}

TEST(RuntimeStatsTest, TracksInflightAndHighWater) {
    RuntimeStats stats;
    stats.RecordInflightAcquired(10);
    stats.RecordInflightAcquired(20);

    RuntimeStatsSnapshot snapshot = stats.Snapshot();
    EXPECT_EQ(snapshot.inflight_records, 2U);
    EXPECT_EQ(snapshot.inflight_bytes, 30U);
    EXPECT_EQ(snapshot.high_water_records, 2U);
    EXPECT_EQ(snapshot.high_water_bytes, 30U);

    EXPECT_TRUE(stats.RecordInflightReleased(20));
    stats.RecordInflightAcquired(5);
    snapshot = stats.Snapshot();
    EXPECT_EQ(snapshot.inflight_records, 2U);
    EXPECT_EQ(snapshot.inflight_bytes, 15U);
    EXPECT_EQ(snapshot.high_water_bytes, 30U);

    EXPECT_TRUE(stats.RecordInflightReleased(10));
    EXPECT_TRUE(stats.RecordInflightReleased(5));
    EXPECT_FALSE(stats.RecordInflightReleased(1));
    EXPECT_EQ(stats.Snapshot().inflight_records, 0U);
    EXPECT_EQ(stats.Snapshot().inflight_bytes, 0U);
}

TEST(RuntimeStatsTest, ConcurrentHighWaterIsContentionSafe) {
    RuntimeStats stats;
    constexpr int kThreadCount = 4;
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&]() {
            stats.RecordInflightAcquired(10);
            ready.fetch_add(1);
            while (!release.load()) {
                std::this_thread::yield();
            }
            EXPECT_TRUE(stats.RecordInflightReleased(10));
        });
    }
    while (ready.load() != kThreadCount) {
        std::this_thread::yield();
    }
    release.store(true);
    for (auto& thread : threads) {
        thread.join();
    }

    const RuntimeStatsSnapshot snapshot = stats.Snapshot();
    EXPECT_EQ(snapshot.inflight_records, 0U);
    EXPECT_GE(snapshot.high_water_records, kThreadCount);
    EXPECT_GE(snapshot.high_water_bytes, kThreadCount * 10U);
}

TEST(RuntimeStatsTest, ReleasesPayloadBytesAndOperationRecordsIndependently) {
    RuntimeStats stats;
    stats.RecordInflightAcquired(1024);
    EXPECT_EQ(stats.Snapshot().inflight_records, 1U);
    EXPECT_EQ(stats.Snapshot().inflight_bytes, 1024U);

    EXPECT_TRUE(stats.RecordPayloadReleased(1024));
    EXPECT_EQ(stats.Snapshot().inflight_records, 1U);
    EXPECT_EQ(stats.Snapshot().inflight_bytes, 0U);

    EXPECT_TRUE(stats.RecordOperationCompleted());
    EXPECT_EQ(stats.Snapshot().inflight_records, 0U);
    EXPECT_FALSE(stats.RecordOperationCompleted());
}

TEST(LatencyHistogramTest, RecordsSamplesAndComputesPercentiles) {
    auto histogram = LatencyHistogram::Create(1, 10000, 3);
    ASSERT_TRUE(histogram.has_value());

    for (std::int64_t value = 1; value <= 1000; ++value) {
        ASSERT_TRUE(histogram.value()->Record(value));
    }

    EXPECT_EQ(histogram.value()->Count(), 1000U);
    EXPECT_EQ(histogram.value()->Min(), 1);
    EXPECT_EQ(histogram.value()->Max(), 1000);
    EXPECT_NEAR(static_cast<double>(histogram.value()->P50()), 500.0, 5.0);
    EXPECT_NEAR(static_cast<double>(histogram.value()->P95()), 950.0, 5.0);
    EXPECT_NEAR(static_cast<double>(histogram.value()->P99()), 990.0, 5.0);
}

TEST(LatencyHistogramTest, RejectsInvalidConfigurationAndOutOfRangeSamples) {
    const auto invalid = LatencyHistogram::Create(0, 100, 3);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);

    auto histogram = LatencyHistogram::Create(1, 100, 3);
    ASSERT_TRUE(histogram.has_value());
    EXPECT_FALSE(histogram.value()->Record(101));
    EXPECT_EQ(histogram.value()->Count(), 0U);
}

TEST(LatencyHistogramTest, SupportsConcurrentRecording) {
    auto histogram = LatencyHistogram::Create(1, 1000, 3);
    ASSERT_TRUE(histogram.has_value());
    constexpr int kThreadCount = 4;
    constexpr int kSamplesPerThread = 250;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&]() {
            for (int sample = 1; sample <= kSamplesPerThread; ++sample) {
                EXPECT_TRUE(histogram.value()->Record(sample));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(histogram.value()->Count(), kThreadCount * kSamplesPerThread);
}

class RecordingSink final : public LoggerSink {
public:
    void Write(LogLevel level, std::string_view category, std::string_view message) override {
        level_ = level;
        category_ = std::string(category);
        message_ = std::string(message);
        ++calls_;
    }

    LogLevel level_ = RODAQ_LOG_OFF;
    std::string category_;
    std::string message_;
    int calls_ = 0;
};

class ThrowingSink final : public LoggerSink {
public:
    void Write(LogLevel, std::string_view, std::string_view) override {
        throw std::runtime_error("sink failure");
    }
};

TEST(LoggerTest, ForwardsLevelCategoryAndMessage) {
    RecordingSink sink;
    const Logger logger(sink);
    logger.Log(RODAQ_LOG_WARN, "runtime.admission", "resource pressure");

    EXPECT_EQ(sink.calls_, 1);
    EXPECT_EQ(sink.level_, RODAQ_LOG_WARN);
    EXPECT_EQ(sink.category_, "runtime.admission");
    EXPECT_EQ(sink.message_, "resource pressure");
}

TEST(LoggerTest, InvalidAndThrowingSinksDoNotPropagateExceptions) {
    const Logger invalid;
    EXPECT_FALSE(invalid.Valid());
    EXPECT_NO_THROW(invalid.Log(RODAQ_LOG_ERROR, "runtime", "ignored"));

    ThrowingSink sink;
    const Logger logger(sink);
    EXPECT_NO_THROW(logger.Log(RODAQ_LOG_ERROR, "runtime", "caught"));
}

}  // namespace
}  // namespace rodaq
