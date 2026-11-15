#include "pipeline/sequence_allocator.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <type_traits>

namespace {

namespace pipeline = rodaq::pipeline;
using rodaq::StatusCode;

static_assert(!std::is_copy_constructible_v<pipeline::SequenceAllocator>);
static_assert(!std::is_move_constructible_v<pipeline::SequenceAllocator>);

TEST(SequenceAllocator, RangeIsUnavailableBeforeActualDurableCompletion) {
    pipeline::SequenceAllocator allocator;
    EXPECT_EQ(allocator.allocate().status().code(), StatusCode::ResourceExhausted);
    ASSERT_TRUE(allocator.begin_refill(1).ok());
    EXPECT_EQ(allocator.allocate().status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(allocator.snapshot().refill_work_id, 1);
    EXPECT_EQ(allocator.begin_refill(2).status().code(), StatusCode::Conflict);
    ASSERT_TRUE(allocator.finish_refill(1, {1, 4}).ok());
    EXPECT_EQ(allocator.allocate().value(), 1);
    EXPECT_EQ(allocator.allocate().value(), 2);
    EXPECT_EQ(allocator.allocate().value(), 3);
    EXPECT_EQ(allocator.allocate().status().code(), StatusCode::ResourceExhausted);
}

TEST(SequenceAllocator, PreparedRangeIsBoundedAndDoesNotStallActiveRangeDuringRefill) {
    pipeline::SequenceAllocator allocator;
    ASSERT_TRUE(allocator.begin_refill(1).ok());
    ASSERT_TRUE(allocator.finish_refill(1, {1, 4}).ok());
    ASSERT_TRUE(allocator.begin_refill(2).ok());
    EXPECT_EQ(allocator.allocate().value(), 1);
    EXPECT_EQ(allocator.allocate().value(), 2);
    ASSERT_TRUE(allocator.finish_refill(2, {4, 7}).ok());
    EXPECT_EQ(allocator.snapshot().active_remaining, 1);
    EXPECT_EQ(allocator.snapshot().prepared_remaining, 3);
    EXPECT_EQ(allocator.begin_refill(3).status().code(), StatusCode::Conflict);
    EXPECT_EQ(allocator.allocate().value(), 3);
    EXPECT_EQ(allocator.allocate().value(), 4);
    EXPECT_TRUE(allocator.begin_refill(3).ok());
    EXPECT_EQ(allocator.allocate().value(), 5);
    EXPECT_EQ(allocator.allocate().value(), 6);
}

TEST(SequenceAllocator, StaleWorkCannotInstallNewRangeOrReleaseActualInflightOwnership) {
    pipeline::SequenceAllocator allocator;
    ASSERT_TRUE(allocator.begin_refill(10).ok());
    EXPECT_EQ(allocator.finish_refill(9, {1, 5}).status().code(), StatusCode::Conflict);
    EXPECT_EQ(
        allocator.finish_failure(9, StatusCode::Unavailable).status().code(), StatusCode::Conflict);
    EXPECT_EQ(allocator.snapshot().refill_work_id, 10);
    ASSERT_TRUE(allocator.finish_failure(10, StatusCode::Unavailable).ok());
    EXPECT_EQ(allocator.begin_refill(10).status().code(), StatusCode::InvalidArgument);
    EXPECT_TRUE(allocator.begin_refill(11).ok());
    EXPECT_EQ(allocator.finish_refill(10, {1, 5}).status().code(), StatusCode::Conflict);
    EXPECT_EQ(allocator.snapshot().refill_work_id, 11);
    ASSERT_TRUE(allocator.finish_refill(11, {5, 10}).ok());
    EXPECT_EQ(allocator.allocate().value(), 5);
}

TEST(SequenceAllocator, FailureKeepsPreviouslyDurableCapacityUntilExhaustionAndRejectsOverlap) {
    pipeline::SequenceAllocator allocator;
    ASSERT_TRUE(allocator.begin_refill(1).ok());
    ASSERT_TRUE(allocator.finish_refill(1, {1, 3}).ok());
    ASSERT_TRUE(allocator.begin_refill(2).ok());
    ASSERT_TRUE(allocator.finish_failure(2, StatusCode::Unavailable).ok());
    EXPECT_EQ(allocator.allocate().value(), 1);
    EXPECT_EQ(allocator.allocate().value(), 2);
    EXPECT_EQ(allocator.allocate().status().code(), StatusCode::Unavailable);
    ASSERT_TRUE(allocator.begin_refill(3).ok());
    EXPECT_EQ(allocator.finish_refill(3, {2, 10}).status().code(), StatusCode::DataLoss);
    EXPECT_EQ(allocator.allocate().status().code(), StatusCode::DataLoss);
    ASSERT_TRUE(allocator.begin_refill(4).ok());
    ASSERT_TRUE(allocator.finish_refill(4, {10, 12}).ok());
    EXPECT_EQ(allocator.allocate().value(), 10);
}

TEST(SequenceAllocator, MaximumSequenceNeverWrapsAndMalformedCompletionCannotPublishCapacity) {
    pipeline::SequenceAllocator allocator;
    ASSERT_TRUE(allocator.begin_refill(1).ok());
    EXPECT_EQ(allocator.finish_refill(1, {0, 10}).status().code(), StatusCode::DataLoss);
    ASSERT_TRUE(allocator.begin_refill(2).ok());
    EXPECT_EQ(allocator.finish_refill(2, {10, 10}).status().code(), StatusCode::DataLoss);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    ASSERT_TRUE(allocator.begin_refill(3).ok());
    ASSERT_TRUE(allocator.finish_refill(3, {maximum - 1, maximum}).ok());
    EXPECT_EQ(allocator.allocate().value(), maximum - 1);
    EXPECT_EQ(allocator.snapshot().active_remaining, 0);
    EXPECT_EQ(allocator.allocate().status().code(), StatusCode::ResourceExhausted);
    ASSERT_TRUE(allocator.begin_refill(4).ok());
    EXPECT_EQ(
        allocator.finish_failure(4, StatusCode::Ok).status().code(), StatusCode::InvalidArgument);
    EXPECT_EQ(allocator.snapshot().refill_work_id, 4);
}

}  // namespace
