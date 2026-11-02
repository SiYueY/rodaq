#include <rodaq/result.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace {

rodaq::Result<int> make_error_result() {
    return nonstd::make_unexpected(
        rodaq::Error(rodaq::ErrorCode::NotFound, "value does not exist"));
}

}  // namespace

TEST(ResultTest, HoldsValueAndError) {
    rodaq::Result<int> value(42);
    ASSERT_TRUE(value.has_value());
    EXPECT_TRUE(static_cast<bool>(value));
    EXPECT_EQ(value.value(), 42);

    auto error = make_error_result();
    ASSERT_FALSE(error.has_value());
    EXPECT_FALSE(static_cast<bool>(error));
    EXPECT_EQ(error.error().code(), rodaq::ErrorCode::NotFound);
    EXPECT_EQ(error.error().message(), "value does not exist");
}

TEST(ResultTest, SupportsCopyMoveAndAssignment) {
    rodaq::Result<int> source(7);
    rodaq::Result<int> copy(source);
    EXPECT_EQ(copy.value(), 7);

    rodaq::Result<int> moved(std::move(copy));
    EXPECT_EQ(moved.value(), 7);

    rodaq::Result<int> assigned(0);
    assigned = source;
    EXPECT_EQ(assigned.value(), 7);

    rodaq::Result<int> move_assigned(0);
    move_assigned = std::move(assigned);
    EXPECT_EQ(move_assigned.value(), 7);
}

TEST(ResultTest, SupportsVoidSuccessAndError) {
    rodaq::Result<void> success;
    EXPECT_TRUE(success.has_value());
    EXPECT_TRUE(static_cast<bool>(success));

    rodaq::Result<void> error(
        nonstd::make_unexpected(rodaq::Error(rodaq::ErrorCode::Timeout, "deadline expired")));
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(error.error().code(), rodaq::ErrorCode::Timeout);
}

TEST(ResultTest, SupportsMoveOnlyValues) {
    rodaq::Result<std::unique_ptr<int>> result(std::make_unique<int>(11));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result.value(), 11);

    auto moved = std::move(result);
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(*moved.value(), 11);
}

TEST(ResultTest, SupportsMonadicOperations) {
    const auto transformed = rodaq::Result<int>(21).transform([](int value) { return value * 2; });
    ASSERT_TRUE(transformed.has_value());
    EXPECT_EQ(transformed.value(), 42);

    const auto chained = rodaq::Result<int>(20).and_then(
        [](int value) { return rodaq::Result<std::string>(std::to_string(value)); });
    ASSERT_TRUE(chained.has_value());
    EXPECT_EQ(chained.value(), "20");

    const auto recovered =
        make_error_result().or_else([](const rodaq::Error&) { return rodaq::Result<int>(99); });
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(recovered.value(), 99);
}
