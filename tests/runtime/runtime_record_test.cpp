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
constexpr BindingHandle kBinding = BindingHandle::FromRaw(12);
constexpr SourceHandle kSource = SourceHandle::FromRaw(14);

struct ReleaseOrderProbe {
    ResourceBudget* budget = nullptr;
    ResourceAmount used_at_release{};
};

void observe_release_order(void* owner_context) noexcept {
    auto* probe = static_cast<ReleaseOrderProbe*>(owner_context);
    probe->used_at_release = probe->budget->Used();
}

TEST(TimestampStateTest, PreservesValueAndPresentZeroWithoutClockMetadata) {
    TimestampState timestamps;
    ASSERT_TRUE(timestamps.Set(0, -7).has_value());
    ASSERT_TRUE(timestamps.Set(1, 0).has_value());

    EXPECT_TRUE(timestamps.Present(0));
    EXPECT_EQ(timestamps.Value(0), -7);

    EXPECT_TRUE(timestamps.Present(1));
    EXPECT_EQ(timestamps.Value(1), 0);

    EXPECT_FALSE(timestamps.Present(2));
    EXPECT_EQ(timestamps.Value(2), std::nullopt);
    EXPECT_EQ(timestamps.Present(4), false);
}

TEST(TimestampStateTest, HandlesSignedValueBoundariesAndRejectsInvalidSlots) {
    TimestampState timestamps;
    ASSERT_TRUE(timestamps.Set(0, std::numeric_limits<std::int64_t>::min()).has_value());
    ASSERT_TRUE(timestamps.Set(1, std::numeric_limits<std::int64_t>::max()).has_value());
    EXPECT_EQ(timestamps.Value(0), std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(timestamps.Value(1), std::numeric_limits<std::int64_t>::max());

    const auto bad_slot = timestamps.Set(4, 1);
    ASSERT_FALSE(bad_slot.has_value());
    EXPECT_EQ(bad_slot.error().code(), ErrorCode::InvalidArgument);
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
    ASSERT_TRUE(timestamps.Set(0, 123).has_value());
    {
        auto record = RuntimeRecord::Create(
            kBinding, 0, kSource, std::uint64_t{9}, timestamps, std::move(payload.value()),
            std::move(reservation.value()));
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(pool.value().UsedBytes(), 8U);
        EXPECT_EQ(budget.Used(), (ResourceAmount{1, 4}));
        EXPECT_EQ(record.value().AcceptanceSequence(), 0U);
        EXPECT_EQ(record.value().Binding(), kBinding);
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
    const std::array<std::uint8_t, 3> bytes{};
    auto payload = pool.value().Acquire(3, PayloadView(bytes.data(), bytes.size()));
    ASSERT_TRUE(payload.has_value());
    ResourceBudget budget(ResourceAmount{2, 6});
    auto reservation = budget.TryReserve(ResourceAmount{1, 3});
    ASSERT_TRUE(reservation.has_value());

    auto created = RuntimeRecord::Create(
        kBinding, 4, std::nullopt, std::nullopt, TimestampState{}, std::move(payload.value()),
        std::move(reservation.value()));
    ASSERT_TRUE(created.has_value());

    RuntimeRecord moved(std::move(created.value()));
    EXPECT_EQ(moved.AcceptanceSequence(), 4U);
    EXPECT_EQ(pool.value().UsedBytes(), 8U);
    EXPECT_EQ(budget.Used(), (ResourceAmount{1, 3}));
}

TEST(RuntimeRecordTest, RejectsSourceSequenceWithoutSource) {
    ResourceBudget budget(ResourceAmount{1, 0});
    auto reservation = budget.TryReserve(ResourceAmount{1, 0});
    ASSERT_TRUE(reservation.has_value());

    const auto invalid = RuntimeRecord::Create(
        kBinding, 0, std::nullopt, std::uint64_t{5}, TimestampState{}, Payload(),
        std::move(reservation.value()));
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(budget.Used(), ResourceAmount{});
}

TEST(RuntimeRecordTest, RejectsInvalidBinding) {
    ResourceBudget budget(ResourceAmount{1, 0});
    auto reservation = budget.TryReserve(ResourceAmount{1, 0});
    ASSERT_TRUE(reservation.has_value());

    const auto invalid = RuntimeRecord::Create(
        BindingHandle{}, 0, std::nullopt, std::nullopt, TimestampState{}, Payload(),
        std::move(reservation.value()));
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST(RuntimeRecordTest, PreservesPresentZeroSourceSequence) {
    ResourceBudget budget(ResourceAmount{1, 0});
    auto reservation = budget.TryReserve(ResourceAmount{1, 0});
    ASSERT_TRUE(reservation.has_value());

    auto record = RuntimeRecord::Create(
        kBinding, 0, kSource, std::uint64_t{0}, TimestampState{}, Payload(),
        std::move(reservation.value()));
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record.value().Source(), kSource);
    EXPECT_EQ(record.value().SourceSequence(), std::uint64_t{0});
}

TEST(RuntimeRecordTest, ReleasesPayloadResponsibilityBeforeBudgetCapacity) {
    const std::array<std::uint8_t, 4> source{1, 2, 3, 4};
    ResourceBudget budget(ResourceAmount{1, 4});
    auto reservation = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(reservation.has_value());
    ReleaseOrderProbe probe;
    probe.budget = &budget;
    auto retained =
        Payload::Retain(PayloadView(source.data(), source.size()), &probe, &observe_release_order);
    ASSERT_TRUE(retained.has_value());

    {
        auto record = RuntimeRecord::Create(
            kBinding, 0, std::nullopt, std::nullopt, TimestampState{}, std::move(retained.value()),
            std::move(reservation.value()));
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(budget.Used(), (ResourceAmount{1, 4}));
    }

    EXPECT_EQ(probe.used_at_release, (ResourceAmount{1, 4}));
    EXPECT_EQ(budget.Used(), ResourceAmount{});
}

static_assert(!std::is_copy_constructible<RuntimeRecord>::value);
static_assert(std::is_nothrow_move_constructible<RuntimeRecord>::value);
static_assert(!std::is_move_assignable<RuntimeRecord>::value);

}  // namespace
}  // namespace rodaq
