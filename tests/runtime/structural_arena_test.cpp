#include <rodaq/runtime/structural_arena.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

namespace rodaq {
namespace {

TEST(StructuralArenaTest, NormalAllocationsAreAlignedAndStable) {
    auto arena = StructuralArena::create(256);
    ASSERT_TRUE(arena.has_value());

    const auto first = arena.value()->allocate<std::uint64_t>();
    const auto second = arena.value()->allocate<double>();
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
    auto arena = StructuralArena::create(64);
    ASSERT_TRUE(arena.has_value());

    const auto allocation = arena.value()->allocate_bytes(64, 1);
    ASSERT_TRUE(allocation.has_value());
    EXPECT_NE(allocation.value(), nullptr);

    const auto exhausted = arena.value()->allocate_bytes(1, 1);
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);
}

TEST(StructuralArenaTest, ExhaustionReturnsResourceExhausted) {
    auto arena = StructuralArena::create(16);
    ASSERT_TRUE(arena.has_value());

    ASSERT_TRUE(arena.value()->allocate_bytes(16, 1).has_value());
    const auto exhausted = arena.value()->allocate_bytes(1, 1);
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);
}

TEST(StructuralArenaTest, InvalidAlignmentAndOverflowAreRejected) {
    auto arena = StructuralArena::create(64);
    ASSERT_TRUE(arena.has_value());

    const auto bad_alignment = arena.value()->allocate_bytes(1, 3);
    ASSERT_FALSE(bad_alignment.has_value());
    EXPECT_EQ(bad_alignment.error().code(), ErrorCode::InvalidArgument);

    const auto overflow =
        arena.value()->allocate<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), ErrorCode::InvalidArgument);
}

TEST(StructuralArenaTest, ZeroSizedAllocationDoesNotConsumeCapacity) {
    auto arena = StructuralArena::create(8);
    ASSERT_TRUE(arena.has_value());

    const auto zero = arena.value()->allocate_bytes(0);
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(zero.value(), nullptr);
    EXPECT_TRUE(arena.value()->allocate_bytes(8, 1).has_value());
}

}  // namespace
}  // namespace rodaq
