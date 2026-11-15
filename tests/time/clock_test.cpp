#include "time/clock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace {

namespace time = rodaq::time;

TEST(BootIdentity, KernelIdentityIsStableAndNeverRandomlyReplaced) {
    auto first = time::parse_boot_identity("12345678-1234-4234-8234-123456789abc");
    auto second = time::parse_boot_identity("12345678-1234-4234-8234-123456789abc\n");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().id, std::uint64_t{390240683298117939ULL});
    EXPECT_EQ(first.value().id, second.value().id);
    EXPECT_EQ(first.value().kernel_uuid, second.value().kernel_uuid);
    for (const std::string_view invalid :
         {"", "random", "00000000-0000-0000-0000-000000000000",
          "12345678-1234-4234-8234-123456789abC", "12345678-1234-4234-8234-123456789abc ",
          "1234567811234-4234-8234-123456789abc"}) {
        EXPECT_FALSE(time::parse_boot_identity(invalid).ok());
    }
    auto kernel_first = time::read_boot_identity();
    auto kernel_second = time::read_boot_identity();
    ASSERT_TRUE(kernel_first.ok());
    ASSERT_TRUE(kernel_second.ok());
    EXPECT_EQ(kernel_first.value().id, kernel_second.value().id);
    EXPECT_EQ(kernel_first.value().kernel_uuid, kernel_second.value().kernel_uuid);
}

TEST(ClockSample, PairedReadingsHaveCheckedMidpointAndConservativeErrorBound) {
    const auto paired = time::pair_clock_readings({1, 100}, {2, 200}, {1, 105});
    ASSERT_TRUE(paired.ok());
    EXPECT_EQ(paired.value().monotonic_ns, 1000000102);
    EXPECT_EQ(paired.value().realtime_ns, 2000000200);
    EXPECT_EQ(paired.value().error_bound_ns, 3);
    EXPECT_FALSE(time::pair_clock_readings({1, 106}, {2, 200}, {1, 105}).ok());
    EXPECT_FALSE(time::pair_clock_readings({-1, 0}, {2, 0}, {1, 0}).ok());
    EXPECT_FALSE(time::pair_clock_readings({0, 0}, {2, 1000000000}, {1, 0}).ok());
    auto sampled = time::sample_clocks();
    ASSERT_TRUE(sampled.ok());
    EXPECT_GT(sampled.value().monotonic_ns, 0);
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
    EXPECT_EQ(negative.value().realtime_ns, -1);
}

TEST(ClockCorrelation, DetectsBothJumpDirectionsAndHonorsMeasuredErrorBudget) {
    const time::ClockSample previous{100, 1000, 1};
    auto normal = time::compare_correlation(previous, {200, 1101, 1}, 0);
    ASSERT_TRUE(normal.ok());
    EXPECT_FALSE(normal.value().discontinuity);
    EXPECT_EQ(normal.value().delta_ns, 1);
    auto forward = time::compare_correlation(previous, {200, 1200, 1}, 10);
    auto backward = time::compare_correlation(previous, {200, 900, 1}, 10);
    ASSERT_TRUE(forward.ok());
    ASSERT_TRUE(backward.ok());
    EXPECT_TRUE(forward.value().discontinuity);
    EXPECT_EQ(forward.value().delta_ns, 100);
    EXPECT_TRUE(backward.value().discontinuity);
    EXPECT_EQ(backward.value().delta_ns, -200);
    EXPECT_FALSE(time::compare_correlation(previous, {200, 1112, 1}, 10).value().discontinuity);
    EXPECT_FALSE(time::compare_correlation(previous, {99, 1100, 1}, 10).ok());
    EXPECT_FALSE(time::compare_correlation(
                     {0, std::numeric_limits<std::int64_t>::min(), 0},
                     {1, std::numeric_limits<std::int64_t>::max(), 0}, 0)
                     .ok());
    EXPECT_FALSE(time::compare_correlation(
                     previous, {200, 1100, std::numeric_limits<std::uint64_t>::max()}, 1)
                     .ok());
}

TEST(PlaybackProjection, UsesImmutableBootAnchorAndRejectsCrossBootComparison) {
    const time::PlaybackAnchor anchor{7, 100, 1000};
    EXPECT_EQ(time::project_playback_time(anchor, 7, 90).value(), 990);
    EXPECT_EQ(time::project_playback_time(anchor, 7, 110).value(), 1010);
    EXPECT_FALSE(time::project_playback_time(anchor, 8, 110).ok());
    EXPECT_FALSE(time::project_playback_time({0, 100, 1000}, 0, 110).ok());
}

TEST(PlaybackProjection, ChecksFullUnsignedMonotonicOffsetsWithoutSignedWrap) {
    constexpr auto kMinimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto kUnsignedMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(time::project_playback_time({1, 0, kMinimum}, 1, kUnsignedMax).value(), kMaximum);
    EXPECT_EQ(time::project_playback_time({1, kUnsignedMax, kMaximum}, 1, 0).value(), kMinimum);
    EXPECT_FALSE(time::project_playback_time({1, 0, 0}, 1, kUnsignedMax).ok());
    EXPECT_FALSE(time::project_playback_time({1, kUnsignedMax, -1}, 1, 0).ok());
    EXPECT_FALSE(time::project_playback_time({1, 0, kMaximum}, 1, 1).ok());
    EXPECT_FALSE(time::project_playback_time({1, 1, kMinimum}, 1, 0).ok());
}

}  // namespace
