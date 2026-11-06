#include "runtime/clock_correlation.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using rodaq::runtime::ClockCorrelation;
using rodaq::runtime::project_unix_nanoseconds;
using rodaq::runtime::UnixEpochClockAnchor;

rodaq_clock_handle clock(std::uint64_t value) noexcept { return rodaq_clock_handle{value}; }

UnixEpochClockAnchor anchor(std::int64_t reading, std::uint64_t unix_ns) {
    return UnixEpochClockAnchor{clock(11), reading, unix_ns};
}

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.ok()) {
        throw std::runtime_error("Expected successful clock operation");
    }
    return std::move(result.value());
}

}  // namespace

TEST(ClockCorrelationTest, ProjectsPositiveNegativeAndEqualDeltas) {
    const UnixEpochClockAnchor base = anchor(100, 1'000);
    EXPECT_EQ(1'000U, take(project_unix_nanoseconds(100, base)));
    EXPECT_EQ(1'025U, take(project_unix_nanoseconds(125, base)));
    EXPECT_EQ(950U, take(project_unix_nanoseconds(50, base)));

    const UnixEpochClockAnchor negative = anchor(-100, 1'000);
    EXPECT_EQ(1'050U, take(project_unix_nanoseconds(-50, negative)));
    EXPECT_EQ(950U, take(project_unix_nanoseconds(-150, negative)));
}

TEST(ClockCorrelationTest, RejectsCheckedArithmeticFailures) {
    const UnixEpochClockAnchor overflow = anchor(0, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        project_unix_nanoseconds(1, overflow).status().status_code());

    const UnixEpochClockAnchor underflow = anchor(0, 0);
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        project_unix_nanoseconds(-1, underflow).status().status_code());

    const UnixEpochClockAnchor extreme =
        anchor(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        project_unix_nanoseconds(std::numeric_limits<std::int64_t>::max(), extreme)
            .status()
            .status_code());
}

TEST(ClockCorrelationTest, CapturesOneStableAnchorWithInjectedReaders) {
    int clock_calls = 0;
    std::int64_t current_clock = 100;
    std::uint64_t current_unix = 10'000;
    const ClockCorrelation correlation = take(ClockCorrelation::establish(
        clock(7),
        [&clock_calls, &current_clock] {
            ++clock_calls;
            const std::int64_t value = current_clock;
            current_clock += 4;
            return value;
        },
        [&current_unix] { return current_unix; }));

    EXPECT_EQ(2, clock_calls);
    const UnixEpochClockAnchor first = take(correlation.get_anchor(clock(7)));
    EXPECT_EQ(7U, first.clock.value);
    EXPECT_EQ(102, first.clock_reading);
    EXPECT_EQ(10'000U, first.unix_anchor_ns);

    current_unix = 99'999;
    const UnixEpochClockAnchor second = take(correlation.get_anchor(clock(7)));
    EXPECT_EQ(first.clock_reading, second.clock_reading);
    EXPECT_EQ(first.unix_anchor_ns, second.unix_anchor_ns);
    EXPECT_EQ(rodaq::StatusCode::NotFound, correlation.get_anchor(clock(8)).status().status_code());
}

TEST(ClockCorrelationTest, CreatesIndependentAnchorsPerActivation) {
    const ClockCorrelation first = take(ClockCorrelation::from_anchor(anchor(10, 1'000)));
    const ClockCorrelation second = take(ClockCorrelation::from_anchor(anchor(20, 2'000)));

    EXPECT_EQ(1'000U, take(first.get_anchor(clock(11))).unix_anchor_ns);
    EXPECT_EQ(2'000U, take(second.get_anchor(clock(11))).unix_anchor_ns);
}

TEST(ClockCorrelationTest, RejectsBackwardAndUnknownClockInputs) {
    int call = 0;
    const auto backward = ClockCorrelation::establish(
        clock(1), [&call] { return call++ == 0 ? 100 : 99; }, [] { return 1'000U; });
    ASSERT_FALSE(backward.ok());
    EXPECT_EQ(rodaq::StatusCode::Unavailable, backward.status().status_code());

    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        ClockCorrelation::from_anchor(UnixEpochClockAnchor{clock(0), 0, 0}).status().status_code());
}

TEST(ClockCorrelationTest, AbiAdapterReturnsStableCallerOwnedAnchor) {
    ClockCorrelation correlation = take(ClockCorrelation::from_anchor(anchor(42, 5'000)));
    const rodaq_interface interface = correlation.abi_interface();
    const auto* vtable = static_cast<const rodaq_runtime_clock_correlation_v1*>(interface.vtable);
    ASSERT_NE(nullptr, vtable);
    EXPECT_EQ(RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION, vtable->protocol_version);
    EXPECT_EQ(RODAQ_RUNTIME_CLOCK_CORRELATION_V1_MIN_STRUCT_SIZE, vtable->struct_size);

    rodaq_unix_epoch_clock_anchor output{};
    output.struct_size = RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE;
    rodaq_status error{};
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

    struct GuardedOutput {
        rodaq_unix_epoch_clock_anchor value;
        std::uint8_t canary[32];
    } guarded{};
    std::memset(guarded.canary, 0xA5, sizeof(guarded.canary));
    guarded.value.struct_size = RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE;
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        vtable->get_unix_epoch_anchor(interface.context, clock(11), &guarded.value, &error));
    for (std::uint8_t byte : guarded.canary) {
        EXPECT_EQ(0xA5, byte);
    }
}
