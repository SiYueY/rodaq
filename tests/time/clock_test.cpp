#include "time/clock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

namespace time = rodaq::time;

TEST(ClockSample, PairedReadingsHaveCheckedMidpointAndConservativeErrorBound) {
    const auto paired = time::pair_clock_readings({1, 100}, {2, 200}, {1, 105});
    ASSERT_TRUE(paired.ok());
    EXPECT_EQ(paired.value().monotonic_ns, 1000000102ULL);
    EXPECT_EQ(paired.value().realtime_ns, 2000000200LL);
    EXPECT_EQ(paired.value().error_bound_ns, 3ULL);
    EXPECT_FALSE(time::pair_clock_readings({1, 106}, {2, 200}, {1, 105}).ok());
    EXPECT_FALSE(time::pair_clock_readings({-1, 0}, {2, 0}, {1, 0}).ok());
    EXPECT_FALSE(time::pair_clock_readings({0, 0}, {2, 1000000000}, {1, 0}).ok());
    auto sampled = time::sample_clocks();
    ASSERT_TRUE(sampled.ok());
    EXPECT_GT(sampled.value().monotonic_ns, 0ULL);
}

TEST(ClockSample, SignedRealtimeNanosecondBoundariesDoNotOverflowIntermediateSeconds) {
    const auto minimum = time::pair_clock_readings({0, 0}, {-9223372037LL, 145224192}, {0, 1});
    const auto maximum = time::pair_clock_readings({0, 0}, {9223372036LL, 854775807}, {0, 1});
    ASSERT_TRUE(minimum.ok());
    ASSERT_TRUE(maximum.ok());
    EXPECT_EQ(minimum.value().realtime_ns, std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(maximum.value().realtime_ns, std::numeric_limits<std::int64_t>::max());
    EXPECT_FALSE(time::pair_clock_readings({0, 0}, {-9223372037LL, 145224191}, {0, 1}).ok());
    EXPECT_FALSE(time::pair_clock_readings({0, 0}, {9223372036LL, 854775808}, {0, 1}).ok());
    const auto negative = time::pair_clock_readings({0, 0}, {-1, 999999999}, {0, 0});
    ASSERT_TRUE(negative.ok());
    EXPECT_EQ(negative.value().realtime_ns, -1LL);
}

TEST(PlaybackProjection, ProjectsMonotonicTimeThroughPerInstanceAnchor) {
    const time::PlaybackAnchor anchor{100, 1000};
    EXPECT_EQ(time::project_recording_time(anchor, 90).value(), 990ULL);
    EXPECT_EQ(time::project_recording_time(anchor, 110).value(), 1010ULL);
    EXPECT_EQ(time::project_recording_time(anchor, 100).value(), 1000ULL);
}

TEST(PlaybackProjection, RejectsNegativeResultAndUnsignedOverflow) {
    constexpr auto kMaxInt64 = std::numeric_limits<std::int64_t>::max();
    constexpr auto kMaxUint64 = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(time::project_recording_time({100, 50}, 0).ok());
    EXPECT_FALSE(time::project_recording_time({100, -10}, 100).ok());
    EXPECT_EQ(time::project_recording_time({0, 0}, kMaxUint64).value(), kMaxUint64);
    EXPECT_FALSE(time::project_recording_time({0, kMaxInt64}, kMaxUint64).ok());
}

}  // namespace
