#include <rodaq/runtime/resource_budget.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace rodaq {
namespace {

TEST(ResourceBudgetTest, ReservationIsAllOrNothing) {
    ResourceBudget budget(ResourceAmount{2, 8});

    auto first = budget.TryReserve(ResourceAmount{1, 6});
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(budget.Used(), (ResourceAmount{1, 6}));

    const auto rejected = budget.TryReserve(ResourceAmount{1, 3});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::ResourceExhausted);
    EXPECT_EQ(budget.Used(), (ResourceAmount{1, 6}));
    EXPECT_EQ(budget.Available(), (ResourceAmount{1, 2}));
}

TEST(ResourceBudgetTest, ReservationReleasesExactlyOnce) {
    ResourceBudget budget(ResourceAmount{3, 12});

    auto reservation = budget.TryReserve(ResourceAmount{2, 8});
    ASSERT_TRUE(reservation.has_value());
    EXPECT_EQ(budget.ActiveReservations(), 1U);

    reservation.value().Release();
    EXPECT_FALSE(reservation.value().Active());
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);

    reservation.value().Release();
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(ResourceBudgetTest, MoveTransfersReservationOwnership) {
    ResourceBudget budget(ResourceAmount{1, 4});

    auto source = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(source.has_value());
    Reservation target(std::move(source.value()));

    EXPECT_FALSE(source.value().Active());
    EXPECT_TRUE(target.Active());
    EXPECT_EQ(budget.Used(), (ResourceAmount{1, 4}));

    target = Reservation();
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(ResourceBudgetTest, MoveAssignmentReleasesPreviousReservation) {
    ResourceBudget budget(ResourceAmount{2, 8});

    auto first = budget.TryReserve(ResourceAmount{1, 4});
    auto second = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(budget.Used(), (ResourceAmount{2, 8}));

    second.value() = std::move(first.value());
    EXPECT_EQ(budget.Used(), (ResourceAmount{1, 4}));
    EXPECT_EQ(budget.ActiveReservations(), 1U);
}

TEST(ResourceBudgetTest, HighWaterTracksPeakUsage) {
    ResourceBudget budget(ResourceAmount{4, 40});

    auto first = budget.TryReserve(ResourceAmount{2, 12});
    auto second = budget.TryReserve(ResourceAmount{1, 20});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(budget.HighWater(), (ResourceAmount{3, 32}));

    first.value().Release();
    second.value().Release();
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.HighWater(), (ResourceAmount{3, 32}));
}

TEST(ResourceBudgetTest, HandlesUnsignedBoundariesWithoutOverflow) {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    ResourceBudget budget(ResourceAmount{maximum, maximum});

    auto reservation = budget.TryReserve(ResourceAmount{maximum - 1U, maximum});
    ASSERT_TRUE(reservation.has_value());
    EXPECT_EQ(budget.Available(), (ResourceAmount{1, 0}));

    const auto rejected = budget.TryReserve(ResourceAmount{2, 0});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(budget.Used(), (ResourceAmount{maximum - 1U, maximum}));

    auto last_record = budget.TryReserve(ResourceAmount{1, 0});
    ASSERT_TRUE(last_record.has_value());
    EXPECT_EQ(budget.Available(), ResourceAmount{});
    EXPECT_EQ(budget.Used(), (ResourceAmount{maximum, maximum}));
}

TEST(ResourceBudgetTest, SaturatedCapacitiesAreNotIntegerWrapped) {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    ResourceBudget budget(ResourceAmount{1, 1});

    const auto rejected = budget.TryReserve(ResourceAmount{maximum, maximum});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(budget.Used(), ResourceAmount{});
}

static_assert(!std::is_copy_constructible<Reservation>::value);
static_assert(std::is_nothrow_move_constructible<Reservation>::value);

}  // namespace
}  // namespace rodaq
