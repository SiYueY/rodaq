#include <rodaq/result.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace {

struct Point {
    int value;
};

template <typename T, typename = void>
struct HasLvalueArrow : std::false_type {};

template <typename T>
struct HasLvalueArrow<T, std::void_t<decltype(std::declval<T&>().operator->())>> : std::true_type {
};

template <typename T, typename = void>
struct HasRvalueArrow : std::false_type {};

template <typename T>
struct HasRvalueArrow<T, std::void_t<decltype(std::declval<T&&>().operator->())>> : std::true_type {
};

template <typename T, typename = void>
struct CanMessageRvalue : std::false_type {};

template <typename T>
struct CanMessageRvalue<T, std::void_t<decltype(std::declval<T&&>().message())>> : std::true_type {
};

static_assert(!std::is_default_constructible_v<rodaq::Result<int>>);
static_assert(std::is_default_constructible_v<rodaq::Result<void>>);
static_assert(!std::is_copy_constructible_v<rodaq::Result<std::unique_ptr<int>>>);
static_assert(std::is_move_constructible_v<rodaq::Result<std::unique_ptr<int>>>);
static_assert(std::is_same_v<decltype(std::declval<rodaq::Result<int>&>().value()), int&>);
static_assert(
    std::is_same_v<decltype(std::declval<const rodaq::Result<int>&>().value()), const int&>);
static_assert(std::is_same_v<decltype(std::declval<rodaq::Result<int>&&>().value()), int&&>);
static_assert(HasLvalueArrow<rodaq::Result<Point>>::value);
static_assert(!HasRvalueArrow<rodaq::Result<Point>>::value);
static_assert(!CanMessageRvalue<rodaq::Status>::value);
static_assert(!std::is_constructible_v<rodaq::Result<bool>, rodaq::Result<void>>);

TEST(ResultTest, HoldsValuesAndPreservesFailureDiagnostics) {
    rodaq::Result<int> value(42);
    ASSERT_TRUE(value.ok());
    EXPECT_TRUE(value.status().ok());
    EXPECT_EQ(42, value.value());
    EXPECT_EQ(42, *value);

    const rodaq::Status error(rodaq::StatusCode::Unavailable, "dds", -12, "transport failed");
    rodaq::Result<int> failure(error);
    ASSERT_FALSE(failure.ok());
    EXPECT_EQ(rodaq::StatusCode::Unavailable, failure.status().code());
    EXPECT_EQ("dds", failure.status().domain());
    EXPECT_EQ(-12, failure.status().error_code());
    EXPECT_EQ("transport failed", failure.status().message());
}

TEST(ResultTest, CopiesMovesAndAssignsValuesAndFailures) {
    rodaq::Result<std::string> source("value");
    auto copy = source;
    auto moved = std::move(copy);
    EXPECT_EQ("value", moved.value());

    source = rodaq::Status(rodaq::StatusCode::Unavailable, "dds", -12, "failed");
    ASSERT_FALSE(source.ok());
    source = std::move(moved);
    ASSERT_TRUE(source.ok());
    EXPECT_EQ("value", source.value());
}

TEST(ResultTest, SupportsMoveOnlyAndPointerLikeAccess) {
    rodaq::Result<std::unique_ptr<int>> owned(std::make_unique<int>(9));
    auto moved = std::move(owned);
    ASSERT_TRUE(moved.ok());
    auto pointer = std::move(moved).value();
    EXPECT_EQ(9, *pointer);

    rodaq::Result<Point> point(Point{7});
    EXPECT_EQ(7, point->value);
}

TEST(ResultTest, VoidDefaultsToSuccessAndPreservesDiagnostics) {
    rodaq::Result<void> success;
    EXPECT_TRUE(success.ok());
    success.value();
    EXPECT_TRUE(success.status().ok());

    rodaq::Result<void> failure(
        rodaq::Status(rodaq::StatusCode::StorageError, "storage", -17, "write failed"));
    ASSERT_FALSE(failure.ok());
    EXPECT_EQ(rodaq::StatusCode::StorageError, failure.status().code());
    EXPECT_EQ("storage", failure.status().domain());
    EXPECT_EQ(-17, failure.status().error_code());

    auto diagnostic = std::move(failure).status();
    EXPECT_EQ("write failed", diagnostic.message());
}

TEST(ResultContractTest, RejectsOkStatusAsFailure) {
    EXPECT_DEATH(
        {
            rodaq::Result<int> invalid(rodaq::Status::Ok());
            static_cast<void>(invalid);
        },
        "");
    EXPECT_DEATH(
        {
            rodaq::Result<void> invalid(rodaq::Status::Ok());
            static_cast<void>(invalid);
        },
        "");
}

TEST(ResultContractTest, InvalidValueAccessAborts) {
    EXPECT_DEATH(
        {
            rodaq::Result<int> invalid(rodaq::Status(rodaq::StatusCode::Timeout, "timeout"));
            static_cast<void>(invalid.value());
        },
        "");
    EXPECT_DEATH(
        {
            rodaq::Result<void> invalid(rodaq::Status(rodaq::StatusCode::Timeout, "timeout"));
            invalid.value();
        },
        "");
}

}  // namespace
