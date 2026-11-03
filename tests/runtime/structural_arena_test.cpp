#include <rodaq/runtime/structural_arena.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace rodaq {
namespace {

TEST(StructuralArenaTest, NormalAllocationsAreAlignedAndStable) {
    auto arena = StructuralArena::Create(256);
    ASSERT_TRUE(arena.has_value());

    const auto first = arena.value()->Allocate<std::uint64_t>();
    const auto second = arena.value()->Allocate<double>();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(first.value()) % alignof(std::uint64_t), 0U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(second.value()) % alignof(double), 0U);

    *first.value() = 7;
    *second.value() = 2.5;
    EXPECT_EQ(*first.value(), 7U);
    EXPECT_DOUBLE_EQ(*second.value(), 2.5);
}

TEST(StructuralArenaTest, ExactCapacityCanBeAllocated) {
    auto arena = StructuralArena::Create(64);
    ASSERT_TRUE(arena.has_value());

    const auto allocation = arena.value()->AllocateBytes(64, 1);
    ASSERT_TRUE(allocation.has_value());
    EXPECT_NE(allocation.value(), nullptr);

    const auto exhausted = arena.value()->AllocateBytes(1, 1);
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);
}

TEST(StructuralArenaTest, ExhaustionReturnsResourceExhausted) {
    auto arena = StructuralArena::Create(16);
    ASSERT_TRUE(arena.has_value());

    ASSERT_TRUE(arena.value()->AllocateBytes(16, 1).has_value());
    const auto exhausted = arena.value()->AllocateBytes(1, 1);
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);
}

TEST(StructuralArenaTest, InvalidAlignmentAndOverflowAreRejected) {
    auto arena = StructuralArena::Create(64);
    ASSERT_TRUE(arena.has_value());

    const auto bad_alignment = arena.value()->AllocateBytes(1, 3);
    ASSERT_FALSE(bad_alignment.has_value());
    EXPECT_EQ(bad_alignment.error().code(), ErrorCode::InvalidArgument);

    const auto overflow =
        arena.value()->Allocate<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), ErrorCode::InvalidArgument);
}

TEST(StructuralArenaTest, ZeroSizedAllocationDoesNotConsumeCapacity) {
    auto arena = StructuralArena::Create(8);
    ASSERT_TRUE(arena.has_value());

    const auto zero = arena.value()->AllocateBytes(0);
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(zero.value(), nullptr);
    EXPECT_TRUE(arena.value()->AllocateBytes(8, 1).has_value());
}

}  // namespace
}  // namespace rodaq
