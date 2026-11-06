#include "admission/resource_budget.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace rodaq {
namespace {

TEST(ResourceBudgetTest, ReservationIsAllOrNothing) {
    ResourceBudget budget(ResourceAmount{2, 8});

    auto first = budget.try_reserve(ResourceAmount{1, 6});
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(budget.used(), (ResourceAmount{1, 6}));

    const auto rejected = budget.try_reserve(ResourceAmount{1, 3});
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.status().status_code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(budget.used(), (ResourceAmount{1, 6}));
    EXPECT_EQ(budget.available(), (ResourceAmount{1, 2}));
}

TEST(ResourceBudgetTest, ReservationReleasesExactlyOnce) {
    ResourceBudget budget(ResourceAmount{3, 12});

    auto reservation = budget.try_reserve(ResourceAmount{2, 8});
    ASSERT_TRUE(reservation.ok());
    EXPECT_EQ(budget.active_reservations(), 1U);

    reservation.value().release();
    EXPECT_FALSE(reservation.value().active());
    EXPECT_EQ(budget.used(), ResourceAmount{});
    EXPECT_EQ(budget.active_reservations(), 0U);

    reservation.value().release();
    EXPECT_EQ(budget.used(), ResourceAmount{});
    EXPECT_EQ(budget.active_reservations(), 0U);
}

TEST(ResourceBudgetTest, MoveTransfersReservationOwnership) {
    ResourceBudget budget(ResourceAmount{1, 4});

    auto source = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(source.ok());
    Reservation target(std::move(source.value()));

    EXPECT_FALSE(source.value().active());
    EXPECT_TRUE(target.active());
    EXPECT_EQ(budget.used(), (ResourceAmount{1, 4}));

    target = Reservation();
    EXPECT_EQ(budget.used(), ResourceAmount{});
    EXPECT_EQ(budget.active_reservations(), 0U);
}

TEST(ResourceBudgetTest, MoveAssignmentReleasesPreviousReservation) {
    ResourceBudget budget(ResourceAmount{2, 8});

    auto first = budget.try_reserve(ResourceAmount{1, 4});
    auto second = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(budget.used(), (ResourceAmount{2, 8}));

    second.value() = std::move(first.value());
    EXPECT_EQ(budget.used(), (ResourceAmount{1, 4}));
    EXPECT_EQ(budget.active_reservations(), 1U);
}

TEST(ResourceBudgetTest, HighWaterTracksPeakUsage) {
    ResourceBudget budget(ResourceAmount{4, 40});

    auto first = budget.try_reserve(ResourceAmount{2, 12});
    auto second = budget.try_reserve(ResourceAmount{1, 20});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(budget.high_water(), (ResourceAmount{3, 32}));

    first.value().release();
    second.value().release();
    EXPECT_EQ(budget.used(), ResourceAmount{});
    EXPECT_EQ(budget.high_water(), (ResourceAmount{3, 32}));
}

TEST(ResourceBudgetTest, HandlesUnsignedBoundariesWithoutOverflow) {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    ResourceBudget budget(ResourceAmount{maximum, maximum});

    auto reservation = budget.try_reserve(ResourceAmount{maximum - 1U, maximum});
    ASSERT_TRUE(reservation.ok());
    EXPECT_EQ(budget.available(), (ResourceAmount{1, 0}));

    const auto rejected = budget.try_reserve(ResourceAmount{2, 0});
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(budget.used(), (ResourceAmount{maximum - 1U, maximum}));

    auto last_record = budget.try_reserve(ResourceAmount{1, 0});
    ASSERT_TRUE(last_record.ok());
    EXPECT_EQ(budget.available(), ResourceAmount{});
    EXPECT_EQ(budget.used(), (ResourceAmount{maximum, maximum}));
}

TEST(ResourceBudgetTest, SaturatedCapacitiesAreNotIntegerWrapped) {
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    ResourceBudget budget(ResourceAmount{1, 1});

    const auto rejected = budget.try_reserve(ResourceAmount{maximum, maximum});
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(budget.used(), ResourceAmount{});
}

static_assert(!std::is_copy_constructible<Reservation>::value);
static_assert(std::is_nothrow_move_constructible<Reservation>::value);

}  // namespace
}  // namespace rodaq
