#include "metrics/latency_histogram.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

namespace rodaq {
namespace {

TEST(LatencyHistogramTest, RecordsSamplesAndComputesPercentiles) {
    auto histogram = LatencyHistogram::create(1, 10000, 3);
    ASSERT_TRUE(histogram.ok());

    for (std::int64_t value = 1; value <= 1000; ++value) {
        ASSERT_TRUE(histogram.value()->record(value));
    }

    EXPECT_EQ(1000U, histogram.value()->count());
    EXPECT_EQ(1, histogram.value()->min());
    EXPECT_EQ(1000, histogram.value()->max());
    EXPECT_NEAR(500.0, static_cast<double>(histogram.value()->p50()), 5.0);
    EXPECT_NEAR(950.0, static_cast<double>(histogram.value()->p95()), 5.0);
    EXPECT_NEAR(990.0, static_cast<double>(histogram.value()->p99()), 5.0);
}

TEST(LatencyHistogramTest, RejectsInvalidConfigurationAndOutOfRangeSamples) {
    const auto invalid = LatencyHistogram::create(0, 100, 3);
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(StatusCode::InvalidArgument, invalid.status().status_code());

    auto histogram = LatencyHistogram::create(1, 100, 3);
    ASSERT_TRUE(histogram.ok());
    EXPECT_FALSE(histogram.value()->record(101));
    EXPECT_EQ(0U, histogram.value()->count());
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

    EXPECT_EQ(kThreadCount * kSamplesPerThread, histogram.value()->count());
}

}  // namespace
}  // namespace rodaq
