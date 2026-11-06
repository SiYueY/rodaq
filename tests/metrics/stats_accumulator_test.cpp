#include "metrics/stats_accumulator.hpp"

#include <rodaq/daq_stats.hpp>

#include <gtest/gtest.h>

#include <atomic>
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
    EXPECT_EQ(2U, snapshot.accepted_records);
    EXPECT_EQ(30U, snapshot.accepted_bytes);
    EXPECT_EQ(1U, snapshot.rejected_records);
    EXPECT_EQ(1U, snapshot.timed_out_records);
    EXPECT_EQ(1U, snapshot.cancelled_records);
    EXPECT_EQ(1U, snapshot.gate_closed_records);
}

TEST(StatsAccumulatorTest, TracksInflightAndHighWater) {
    StatsAccumulator stats;
    stats.record_inflight_acquired(10);
    stats.record_inflight_acquired(20);

    DAQStats snapshot = stats.snapshot();
    EXPECT_EQ(2U, snapshot.inflight_records);
    EXPECT_EQ(30U, snapshot.inflight_bytes);
    EXPECT_EQ(2U, snapshot.high_water_records);
    EXPECT_EQ(30U, snapshot.high_water_bytes);

    EXPECT_TRUE(stats.record_inflight_released(20));
    stats.record_inflight_acquired(5);
    snapshot = stats.snapshot();
    EXPECT_EQ(2U, snapshot.inflight_records);
    EXPECT_EQ(15U, snapshot.inflight_bytes);
    EXPECT_EQ(30U, snapshot.high_water_bytes);

    EXPECT_TRUE(stats.record_inflight_released(10));
    EXPECT_TRUE(stats.record_inflight_released(5));
    EXPECT_FALSE(stats.record_inflight_released(1));
    EXPECT_EQ(0U, stats.snapshot().inflight_records);
    EXPECT_EQ(0U, stats.snapshot().inflight_bytes);
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
    EXPECT_EQ(0U, snapshot.inflight_records);
    EXPECT_GE(snapshot.high_water_records, kThreadCount);
    EXPECT_GE(snapshot.high_water_bytes, kThreadCount * 10U);
}

TEST(StatsAccumulatorTest, ReleasesPayloadBytesAndOperationRecordsIndependently) {
    StatsAccumulator stats;
    stats.record_inflight_acquired(1024);
    EXPECT_EQ(1U, stats.snapshot().inflight_records);
    EXPECT_EQ(1024U, stats.snapshot().inflight_bytes);

    EXPECT_TRUE(stats.record_payload_released(1024));
    EXPECT_EQ(1U, stats.snapshot().inflight_records);
    EXPECT_EQ(0U, stats.snapshot().inflight_bytes);

    EXPECT_TRUE(stats.record_operation_completed());
    EXPECT_EQ(0U, stats.snapshot().inflight_records);
    EXPECT_FALSE(stats.record_operation_completed());
}

}  // namespace
}  // namespace rodaq
