#include <rodaq/error.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

namespace {

rodaq::Error make_error_with_temporary_strings() {
    return rodaq::Error(
        rodaq::ErrorCode::Unavailable, std::string("dds"), -12,
        std::string("transport unavailable"));
}

}  // namespace

TEST(ErrorTest, ErrorCodeMatchesStableTaxonomyValues) {
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::InvalidArgument), 1U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::InvalidState), 2U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::NotFound), 3U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::Conflict), 4U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::ResourceExhausted), 5U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::Unavailable), 6U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::StorageError), 7U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::Timeout), 8U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::ErrorCode::InternalError), 9U);
}

TEST(ErrorTest, TwoArgumentConstructorUsesAbsentNativeDetails) {
    const rodaq::Error error(rodaq::ErrorCode::NotFound, "stream does not exist");

    EXPECT_EQ(error.code(), rodaq::ErrorCode::NotFound);
    EXPECT_EQ(error.native_code(), 0);
    EXPECT_TRUE(error.domain().empty());
    EXPECT_EQ(error.message(), "stream does not exist");
}

TEST(ErrorTest, FullConstructorPreservesOwnedFields) {
    const auto error = make_error_with_temporary_strings();

    EXPECT_EQ(error.code(), rodaq::ErrorCode::Unavailable);
    EXPECT_EQ(error.native_code(), -12);
    EXPECT_EQ(error.domain(), "dds");
    EXPECT_EQ(error.message(), "transport unavailable");
}

TEST(ErrorTest, EmptyDiagnosticsArePreserved) {
    const rodaq::Error error(rodaq::ErrorCode::InvalidArgument, "", 0, "");

    EXPECT_TRUE(error.domain().empty());
    EXPECT_TRUE(error.message().empty());
}

TEST(ErrorTest, CopyAndMovePreserveValues) {
    const rodaq::Error original(rodaq::ErrorCode::StorageError, "mcap", 27, "write failed");

    const rodaq::Error copy(original);
    EXPECT_EQ(copy.code(), original.code());
    EXPECT_EQ(copy.native_code(), original.native_code());
    EXPECT_EQ(copy.domain(), original.domain());
    EXPECT_EQ(copy.message(), original.message());

    rodaq::Error copy_source(original);
    rodaq::Error moved(std::move(copy_source));
    EXPECT_EQ(moved.code(), original.code());
    EXPECT_EQ(moved.native_code(), original.native_code());
    EXPECT_EQ(moved.domain(), original.domain());
    EXPECT_EQ(moved.message(), original.message());
}
