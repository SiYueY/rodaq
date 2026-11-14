#include <rodaq/result.hpp>

#include <gtest/gtest.h>

TEST(BuildContract, ResultPreservesFailure) {
    const rodaq::Result<int> result(rodaq::Status(rodaq::StatusCode::InvalidArgument, "input"));
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), rodaq::StatusCode::InvalidArgument);
    EXPECT_EQ(result.status().message(), "input");
}
