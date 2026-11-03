#include <rodaq/runtime/runtime_record.hpp>
#include <rodaq/runtime/payload_pool.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>

namespace rodaq {
namespace {

constexpr StreamHandle kStream = StreamHandle::FromRaw(11);
constexpr StreamDescriptorHandle kDescriptor = StreamDescriptorHandle::FromRaw(12);
constexpr ClockHandle kClock = ClockHandle::FromRaw(13);
constexpr SourceHandle kSource = SourceHandle::FromRaw(14);

TEST(TimestampStateTest, PreservesClockIdentityRawValueAndPresentZero) {
    TimestampState timestamps;
    ASSERT_TRUE(timestamps.Set(0, kClock, -7).has_value());
    ASSERT_TRUE(timestamps.Set(1, ClockHandle::FromRaw(15), 0).has_value());

    EXPECT_TRUE(timestamps.Present(0));
    ASSERT_NE(timestamps.Reading(0), nullptr);
    EXPECT_EQ(timestamps.Reading(0)->clock, kClock);
    EXPECT_EQ(timestamps.Reading(0)->value, -7);

    EXPECT_TRUE(timestamps.Present(1));
    ASSERT_NE(timestamps.Reading(1), nullptr);
    EXPECT_EQ(timestamps.Reading(1)->value, 0);

    EXPECT_FALSE(timestamps.Present(2));
    EXPECT_EQ(timestamps.Reading(2), nullptr);
    EXPECT_EQ(timestamps.Present(4), false);
}

TEST(TimestampStateTest, RejectsInvalidSlotsAndClockHandles) {
    TimestampState timestamps;
    const auto bad_slot = timestamps.Set(4, kClock, 1);
    ASSERT_FALSE(bad_slot.has_value());
    EXPECT_EQ(bad_slot.error().code(), ErrorCode::InvalidArgument);

    const auto bad_clock = timestamps.Set(0, ClockHandle{}, 1);
    ASSERT_FALSE(bad_clock.has_value());
    EXPECT_EQ(bad_clock.error().code(), ErrorCode::InvalidArgument);
}

TEST(AcceptanceSequenceAllocatorTest, SequencesStartAtZeroPerStream) {
    auto allocator = AcceptanceSequenceAllocator::Create(4);
    ASSERT_TRUE(allocator.has_value());

    const auto first = allocator.value().Allocate(kStream);
    const auto second = allocator.value().Allocate(kStream);
    const auto other = allocator.value().Allocate(StreamHandle::FromRaw(99));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(first.value(), 0U);
    EXPECT_EQ(second.value(), 1U);
    EXPECT_EQ(other.value(), 0U);
    EXPECT_EQ(allocator.value().RegisteredStreamCount(), 2U);
}

TEST(AcceptanceSequenceAllocatorTest, RejectedAttemptCanLeaveAPersistentGap) {
    auto allocator = AcceptanceSequenceAllocator::Create(1);
    ASSERT_TRUE(allocator.has_value());

    const auto first = allocator.value().Allocate(kStream);
    const auto rejected_attempt = allocator.value().Allocate(kStream);
    const auto third = allocator.value().Allocate(kStream);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(rejected_attempt.has_value());
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(first.value(), 0U);
    EXPECT_EQ(rejected_attempt.value(), 1U);
    EXPECT_EQ(third.value(), 2U);
}

TEST(AcceptanceSequenceAllocatorTest, ExhaustionIsTerminalForTheStream) {
    auto allocator =
        AcceptanceSequenceAllocator::Create(1, std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(allocator.has_value());

    const auto last = allocator.value().Allocate(kStream);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last.value(), std::numeric_limits<std::uint64_t>::max());

    const auto exhausted = allocator.value().Allocate(kStream);
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);

    const auto still_exhausted = allocator.value().Allocate(kStream);
    ASSERT_FALSE(still_exhausted.has_value());
    EXPECT_EQ(still_exhausted.error().code(), ErrorCode::ResourceExhausted);
}

TEST(AcceptanceSequenceAllocatorTest, StreamCountIsBounded) {
    auto allocator = AcceptanceSequenceAllocator::Create(1);
    ASSERT_TRUE(allocator.has_value());
    ASSERT_TRUE(allocator.value().Allocate(kStream).has_value());

    const auto full = allocator.value().Allocate(StreamHandle::FromRaw(2));
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error().code(), ErrorCode::ResourceExhausted);
}

TEST(RuntimeRecordTest, OwnsPayloadAndReservationAndReleasesBoth) {
    auto pool = PayloadPool::Create(8, 1);
    ASSERT_TRUE(pool.has_value());
    const std::array<std::uint8_t, 4> source{1, 2, 3, 4};
    auto payload = pool.value().Acquire(source.size(), PayloadView(source.data(), source.size()));
    ASSERT_TRUE(payload.has_value());
    ResourceBudget budget(ResourceAmount{1, 4});
    auto reservation = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(reservation.has_value());

    TimestampState timestamps;
    ASSERT_TRUE(timestamps.Set(0, kClock, 123).has_value());
    {
        auto record = RuntimeRecord::Create(
            kStream, kDescriptor, 0, kSource, std::uint64_t{9}, timestamps,
            std::move(payload.value()), std::move(reservation.value()));
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(pool.value().UsedBytes(), 8U);
        EXPECT_EQ(budget.Used(), (ResourceAmount{1, 4}));
        EXPECT_EQ(record.value().AcceptanceSequence(), 0U);
        EXPECT_EQ(record.value().Stream(), kStream);
        EXPECT_EQ(record.value().StreamDescriptor(), kDescriptor);
        EXPECT_EQ(record.value().Source(), kSource);
        EXPECT_EQ(record.value().SourceSequence(), 9U);
        EXPECT_EQ(record.value().RecordPayload().Size(), 4U);
    }

    EXPECT_EQ(pool.value().UsedBytes(), 0U);
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(RuntimeRecordTest, MoveTransfersBothOwnerships) {
    auto pool = PayloadPool::Create(8, 1);
    ASSERT_TRUE(pool.has_value());
    auto payload = pool.value().Acquire(3);
    ASSERT_TRUE(payload.has_value());
    ResourceBudget budget(ResourceAmount{2, 6});
    auto reservation = budget.TryReserve(ResourceAmount{1, 3});
    ASSERT_TRUE(reservation.has_value());

    auto created = RuntimeRecord::Create(
        kStream, kDescriptor, 4, std::nullopt, std::nullopt, TimestampState{},
        std::move(payload.value()), std::move(reservation.value()));
    ASSERT_TRUE(created.has_value());

    RuntimeRecord moved(std::move(created.value()));
    EXPECT_EQ(moved.AcceptanceSequence(), 4U);
    EXPECT_EQ(pool.value().UsedBytes(), 8U);
    EXPECT_EQ(budget.Used(), (ResourceAmount{1, 3}));

    auto target_reservation = budget.TryReserve(ResourceAmount{1, 0});
    ASSERT_TRUE(target_reservation.has_value());
    auto target_result = RuntimeRecord::Create(
        kStream, kDescriptor, 5, std::nullopt, std::nullopt, TimestampState{}, Payload(),
        std::move(target_reservation.value()));
    ASSERT_TRUE(target_result.has_value());
    RuntimeRecord target(std::move(target_result.value()));
    target = std::move(moved);
    EXPECT_EQ(target.AcceptanceSequence(), 4U);
    EXPECT_EQ(budget.Used(), (ResourceAmount{1, 3}));
}

TEST(RuntimeRecordTest, RejectsSourceSequenceWithoutSource) {
    ResourceBudget budget(ResourceAmount{1, 0});
    auto reservation = budget.TryReserve(ResourceAmount{1, 0});
    ASSERT_TRUE(reservation.has_value());

    const auto invalid = RuntimeRecord::Create(
        kStream, kDescriptor, 0, std::nullopt, std::uint64_t{5}, TimestampState{}, Payload(),
        std::move(reservation.value()));
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(budget.Used(), ResourceAmount{});
}

static_assert(!std::is_copy_constructible<RuntimeRecord>::value);
static_assert(std::is_nothrow_move_constructible<RuntimeRecord>::value);

}  // namespace
}  // namespace rodaq
