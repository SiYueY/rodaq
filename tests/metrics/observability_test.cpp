#include "metrics/latency_histogram.hpp"
#include "metrics/stats_accumulator.hpp"
#include "logging/logger.hpp"
#include <rodaq/daq_stats.hpp>

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

TEST(StatsAccumulatorTest, RecordsCountersAndSnapshot) {
    StatsAccumulator stats;
    stats.record_accepted(10);
    stats.record_accepted(20);
    stats.record_rejected();
    stats.record_timed_out();
    stats.record_cancelled();
    stats.record_gate_closed();

    const DAQStats snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.accepted_records, 2U);
    EXPECT_EQ(snapshot.accepted_bytes, 30U);
    EXPECT_EQ(snapshot.rejected_records, 1U);
    EXPECT_EQ(snapshot.timed_out_records, 1U);
    EXPECT_EQ(snapshot.cancelled_records, 1U);
    EXPECT_EQ(snapshot.gate_closed_records, 1U);
}

TEST(StatsAccumulatorTest, TracksInflightAndHighWater) {
    StatsAccumulator stats;
    stats.record_inflight_acquired(10);
    stats.record_inflight_acquired(20);

    DAQStats snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.inflight_records, 2U);
    EXPECT_EQ(snapshot.inflight_bytes, 30U);
    EXPECT_EQ(snapshot.high_water_records, 2U);
    EXPECT_EQ(snapshot.high_water_bytes, 30U);

    EXPECT_TRUE(stats.record_inflight_released(20));
    stats.record_inflight_acquired(5);
    snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.inflight_records, 2U);
    EXPECT_EQ(snapshot.inflight_bytes, 15U);
    EXPECT_EQ(snapshot.high_water_bytes, 30U);

    EXPECT_TRUE(stats.record_inflight_released(10));
    EXPECT_TRUE(stats.record_inflight_released(5));
    EXPECT_FALSE(stats.record_inflight_released(1));
    EXPECT_EQ(stats.snapshot().inflight_records, 0U);
    EXPECT_EQ(stats.snapshot().inflight_bytes, 0U);
}

TEST(StatsAccumulatorTest, ConcurrentHighWaterIsContentionSafe) {
    StatsAccumulator stats;
    constexpr int kThreadCount = 4;
    std::atomic<int> ready{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&]() {
            stats.record_inflight_acquired(10);
            ready.fetch_add(1);
            while (!release.load()) {
                std::this_thread::yield();
            }
            EXPECT_TRUE(stats.record_inflight_released(10));
        });
    }
    while (ready.load() != kThreadCount) {
        std::this_thread::yield();
    }
    release.store(true);
    for (auto& thread : threads) {
        thread.join();
    }

    const DAQStats snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.inflight_records, 0U);
    EXPECT_GE(snapshot.high_water_records, kThreadCount);
    EXPECT_GE(snapshot.high_water_bytes, kThreadCount * 10U);
}

TEST(StatsAccumulatorTest, ReleasesPayloadBytesAndOperationRecordsIndependently) {
    StatsAccumulator stats;
    stats.record_inflight_acquired(1024);
    EXPECT_EQ(stats.snapshot().inflight_records, 1U);
    EXPECT_EQ(stats.snapshot().inflight_bytes, 1024U);

    EXPECT_TRUE(stats.record_payload_released(1024));
    EXPECT_EQ(stats.snapshot().inflight_records, 1U);
    EXPECT_EQ(stats.snapshot().inflight_bytes, 0U);

    EXPECT_TRUE(stats.record_operation_completed());
    EXPECT_EQ(stats.snapshot().inflight_records, 0U);
    EXPECT_FALSE(stats.record_operation_completed());
}

TEST(LatencyHistogramTest, RecordsSamplesAndComputesPercentiles) {
    auto histogram = LatencyHistogram::create(1, 10000, 3);
    ASSERT_TRUE(histogram.ok());

    for (std::int64_t value = 1; value <= 1000; ++value) {
        ASSERT_TRUE(histogram.value()->record(value));
    }

    EXPECT_EQ(histogram.value()->count(), 1000U);
    EXPECT_EQ(histogram.value()->min(), 1);
    EXPECT_EQ(histogram.value()->max(), 1000);
    EXPECT_NEAR(static_cast<double>(histogram.value()->p50()), 500.0, 5.0);
    EXPECT_NEAR(static_cast<double>(histogram.value()->p95()), 950.0, 5.0);
    EXPECT_NEAR(static_cast<double>(histogram.value()->p99()), 990.0, 5.0);
}

TEST(LatencyHistogramTest, RejectsInvalidConfigurationAndOutOfRangeSamples) {
    const auto invalid = LatencyHistogram::create(0, 100, 3);
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().status_code(), StatusCode::InvalidArgument);

    auto histogram = LatencyHistogram::create(1, 100, 3);
    ASSERT_TRUE(histogram.ok());
    EXPECT_FALSE(histogram.value()->record(101));
    EXPECT_EQ(histogram.value()->count(), 0U);
}

TEST(LatencyHistogramTest, SupportsConcurrentRecording) {
    auto histogram = LatencyHistogram::create(1, 1000, 3);
    ASSERT_TRUE(histogram.ok());
    constexpr int kThreadCount = 4;
    constexpr int kSamplesPerThread = 250;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int index = 0; index < kThreadCount; ++index) {
        threads.emplace_back([&]() {
            for (int sample = 1; sample <= kSamplesPerThread; ++sample) {
                EXPECT_TRUE(histogram.value()->record(sample));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(histogram.value()->count(), kThreadCount * kSamplesPerThread);
}

class RecordingSink final : public LoggerSink {
public:
    void write(LogLevel level, std::string_view category, std::string_view message) override {
        level_ = level;
        category_ = std::string(category);
        message_ = std::string(message);
        ++calls_;
    }

    LogLevel level_ = LogLevel::Off;
    std::string category_;
    std::string message_;
    int calls_ = 0;
};

class ThrowingSink final : public LoggerSink {
public:
    void write(LogLevel, std::string_view, std::string_view) override {
        throw std::runtime_error("sink failure");
    }
};

TEST(LoggerTest, ForwardsLevelCategoryAndMessage) {
    RecordingSink sink;
    const Logger logger(sink);
    logger.log(LogLevel::Warn, "runtime.admission", "resource pressure");

    EXPECT_EQ(sink.calls_, 1);
    EXPECT_EQ(sink.level_, LogLevel::Warn);
    EXPECT_EQ(sink.category_, "runtime.admission");
    EXPECT_EQ(sink.message_, "resource pressure");
}

TEST(LoggerTest, InvalidAndThrowingSinksDoNotPropagateExceptions) {
    const Logger invalid;
    EXPECT_FALSE(invalid.valid());
    EXPECT_NO_THROW(invalid.log(LogLevel::Error, "runtime", "ignored"));

    ThrowingSink sink;
    const Logger logger(sink);
    EXPECT_NO_THROW(logger.log(LogLevel::Error, "runtime", "caught"));
}

}  // namespace
}  // namespace rodaq
