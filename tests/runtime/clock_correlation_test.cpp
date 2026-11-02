#include "runtime/clock_correlation.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using rodaq::runtime::ClockCorrelation;
using rodaq::runtime::ProjectUnixNanoseconds;
using rodaq::runtime::UnixEpochClockAnchor;

rodaq_clock_handle clock(std::uint64_t value) noexcept { return rodaq_clock_handle{value}; }

UnixEpochClockAnchor anchor(std::int64_t reading, std::uint64_t unix_ns) {
    return UnixEpochClockAnchor{clock(11), reading, unix_ns};
}

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.has_value()) {
        throw std::runtime_error("Expected successful clock operation");
    }
    return std::move(result.value());
}

}  // namespace

TEST(ClockCorrelationTest, ProjectsPositiveNegativeAndEqualDeltas) {
    const UnixEpochClockAnchor base = anchor(100, 1'000);
    EXPECT_EQ(1'000U, take(ProjectUnixNanoseconds(100, base)));
    EXPECT_EQ(1'025U, take(ProjectUnixNanoseconds(125, base)));
    EXPECT_EQ(950U, take(ProjectUnixNanoseconds(50, base)));

    const UnixEpochClockAnchor negative = anchor(-100, 1'000);
    EXPECT_EQ(1'050U, take(ProjectUnixNanoseconds(-50, negative)));
    EXPECT_EQ(950U, take(ProjectUnixNanoseconds(-150, negative)));
}

TEST(ClockCorrelationTest, RejectsCheckedArithmeticFailures) {
    const UnixEpochClockAnchor overflow = anchor(0, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument, ProjectUnixNanoseconds(1, overflow).error().code());

    const UnixEpochClockAnchor underflow = anchor(0, 0);
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument, ProjectUnixNanoseconds(-1, underflow).error().code());

    const UnixEpochClockAnchor extreme =
        anchor(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        ProjectUnixNanoseconds(std::numeric_limits<std::int64_t>::max(), extreme).error().code());
}

TEST(ClockCorrelationTest, CapturesOneStableAnchorWithInjectedReaders) {
    int clock_calls = 0;
    std::int64_t current_clock = 100;
    std::uint64_t current_unix = 10'000;
    const ClockCorrelation correlation = take(ClockCorrelation::Establish(
        clock(7),
        [&clock_calls, &current_clock] {
            ++clock_calls;
            const std::int64_t value = current_clock;
            current_clock += 4;
            return value;
        },
        [&current_unix] { return current_unix; }));

    EXPECT_EQ(2, clock_calls);
    const UnixEpochClockAnchor first = take(correlation.GetAnchor(clock(7)));
    EXPECT_EQ(7U, first.clock.value);
    EXPECT_EQ(102, first.clock_reading);
    EXPECT_EQ(10'000U, first.unix_anchor_ns);

    current_unix = 99'999;
    const UnixEpochClockAnchor second = take(correlation.GetAnchor(clock(7)));
    EXPECT_EQ(first.clock_reading, second.clock_reading);
    EXPECT_EQ(first.unix_anchor_ns, second.unix_anchor_ns);
    EXPECT_EQ(rodaq::ErrorCode::NotFound, correlation.GetAnchor(clock(8)).error().code());
}

TEST(ClockCorrelationTest, CreatesIndependentAnchorsPerActivation) {
    const ClockCorrelation first = take(ClockCorrelation::FromAnchor(anchor(10, 1'000)));
    const ClockCorrelation second = take(ClockCorrelation::FromAnchor(anchor(20, 2'000)));

    EXPECT_EQ(1'000U, take(first.GetAnchor(clock(11))).unix_anchor_ns);
    EXPECT_EQ(2'000U, take(second.GetAnchor(clock(11))).unix_anchor_ns);
}

TEST(ClockCorrelationTest, RejectsBackwardAndUnknownClockInputs) {
    int call = 0;
    const auto backward = ClockCorrelation::Establish(
        clock(1), [&call] { return call++ == 0 ? 100 : 99; }, [] { return 1'000U; });
    ASSERT_FALSE(backward.has_value());
    EXPECT_EQ(rodaq::ErrorCode::Unavailable, backward.error().code());

    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        ClockCorrelation::FromAnchor(UnixEpochClockAnchor{clock(0), 0, 0}).error().code());
}

TEST(ClockCorrelationTest, AbiAdapterReturnsStableCallerOwnedAnchor) {
    ClockCorrelation correlation = take(ClockCorrelation::FromAnchor(anchor(42, 5'000)));
    const rodaq_interface interface = correlation.AbiInterface();
    const auto* vtable = static_cast<const rodaq_runtime_clock_correlation_v1*>(interface.vtable);
    ASSERT_NE(nullptr, vtable);
    EXPECT_EQ(RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION, vtable->protocol_version);
    EXPECT_EQ(RODAQ_RUNTIME_CLOCK_CORRELATION_V1_MIN_STRUCT_SIZE, vtable->struct_size);

    rodaq_unix_epoch_clock_anchor output{};
    output.struct_size = RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE;
    rodaq_error error{};
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        vtable->get_unix_epoch_anchor(interface.context, clock(11), &output, &error));
    EXPECT_EQ(sizeof(rodaq_unix_epoch_clock_anchor), output.struct_size);
    EXPECT_EQ(0U, output.reserved);
    EXPECT_EQ(42, output.clock_reading);
    EXPECT_EQ(5'000U, output.unix_anchor_ns);

    EXPECT_EQ(
        RODAQ_STATUS_NOT_FOUND,
        vtable->get_unix_epoch_anchor(interface.context, clock(12), &output, &error));
    EXPECT_EQ(0U, output.struct_size);

    output.struct_size = RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE - 1;
    EXPECT_EQ(
        RODAQ_STATUS_INVALID_ARGUMENT,
        vtable->get_unix_epoch_anchor(interface.context, clock(11), &output, &error));
    EXPECT_EQ(0U, output.struct_size);
}
