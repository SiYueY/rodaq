#include <rodaq/result.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

TEST(ResultTest, HoldsValuesAndNonOkStatuses) {
    rodaq::Result<int> value = 42;
    ASSERT_TRUE(value.ok());
    EXPECT_TRUE(value.status().ok());
    EXPECT_EQ(value.value(), 42);
    rodaq::Result<int> failure = rodaq::Status(rodaq::StatusCode::NotFound, "missing");
    ASSERT_FALSE(failure.ok());
    EXPECT_EQ(failure.status().status_code(), rodaq::StatusCode::NotFound);
    EXPECT_EQ(failure.status().message(), "missing");
}

TEST(ResultTest, CopiesMovesAndAssignsValuesAndFailures) {
    rodaq::Result<std::string> source = std::string("value");
    auto copy = source;
    auto moved = std::move(copy);
    EXPECT_EQ(moved.value(), "value");
    source = rodaq::Status(rodaq::StatusCode::Unavailable, "dds", -12, "failed");
    auto failure = source;
    ASSERT_FALSE(failure.ok());
    EXPECT_EQ(failure.status().error_code(), -12);
    EXPECT_EQ(failure.status().domain(), "dds");
    failure = std::move(moved);
    ASSERT_TRUE(failure.ok());
    EXPECT_EQ(failure.value(), "value");
    EXPECT_TRUE(failure.status().ok());
}

TEST(ResultTest, SupportsMoveOnlyValues) {
    rodaq::Result<std::unique_ptr<int>> owned = std::make_unique<int>(9);
    auto moved = std::move(owned);
    ASSERT_TRUE(moved.ok());
    auto pointer = std::move(moved).value();
    EXPECT_EQ(*pointer, 9);
}

TEST(ResultTest, NormalizesOkFailureAndUninitializedResults) {
    rodaq::Result<int> invalid{rodaq::Status()};
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().status_code(), rodaq::StatusCode::UnknownError);
    rodaq::Result<int> uninitialized;
    EXPECT_FALSE(uninitialized.ok());
    invalid = 3;
    ASSERT_TRUE(invalid.ok());
    invalid = rodaq::Status();
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().status_code(), rodaq::StatusCode::UnknownError);
}

static_assert(std::is_const_v<
              std::remove_reference_t<decltype(std::declval<rodaq::Result<int>&>().status())>>);

TEST(ResultTest, VoidDefaultsToSuccessAndHasNoMutableFailureAccess) {
    rodaq::Result<void> result;
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_TRUE(result.status().ok());
    result.value();
    auto copy = result;
    auto moved = std::move(copy);
    EXPECT_TRUE(moved.ok());
    EXPECT_TRUE(std::move(moved).status().ok());
}

TEST(ResultTest, VoidPreservesDiagnosticsAcrossCopyMoveAndAssignment) {
    rodaq::Result<void> failure =
        rodaq::Status(rodaq::StatusCode::StorageError, "storage", -17, "write failed");
    ASSERT_FALSE(failure.ok());
    EXPECT_FALSE(static_cast<bool>(failure));
    auto copy = failure;
    auto moved = std::move(copy);
    EXPECT_EQ(moved.status().status_code(), rodaq::StatusCode::StorageError);
    EXPECT_EQ(moved.status().domain(), "storage");
    EXPECT_EQ(moved.status().error_code(), -17);
    EXPECT_EQ(moved.status().message(), "write failed");
    rodaq::Result<void> assigned;
    assigned = failure;
    EXPECT_FALSE(assigned.ok());
    assigned = rodaq::Result<void>();
    EXPECT_TRUE(assigned.ok());
    assigned = std::move(failure);
    EXPECT_FALSE(assigned.ok());
    const auto diagnostic = std::move(assigned).status();
    EXPECT_EQ(diagnostic.message(), "write failed");
    EXPECT_EQ(diagnostic.error_code(), -17);
}

TEST(ResultTest, VoidRejectsOkAsFailureAndNormalizesUnknownCodes) {
    rodaq::Result<void> invalid{rodaq::Status()};
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().status_code(), rodaq::StatusCode::UnknownError);
    rodaq::Result<void> unknown =
        rodaq::Status(static_cast<rodaq::StatusCode>(999), "backend", 23, "unknown");
    ASSERT_FALSE(unknown.ok());
    EXPECT_EQ(unknown.status().status_code(), rodaq::StatusCode::UnknownError);
    EXPECT_EQ(unknown.status().error_code(), 23);
    invalid = rodaq::Result<void>();
    ASSERT_TRUE(invalid.ok());
    invalid = rodaq::Status();
    EXPECT_FALSE(invalid.ok());
}

static_assert(
    std::is_same_v<decltype(std::declval<rodaq::Result<void>&>().status()), const rodaq::Status&>);
static_assert(std::is_same_v<decltype(std::declval<const rodaq::Result<void>&>().value()), void>);
static_assert(std::is_copy_constructible_v<rodaq::Result<void>>);
static_assert(std::is_move_constructible_v<rodaq::Result<void>>);

static_assert(!std::is_constructible_v<rodaq::Result<bool>, rodaq::Result<void>>);
