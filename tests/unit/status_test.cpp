#include <rodaq/status.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>

namespace {

rodaq::Status make_status_with_temporary_strings() {
    return rodaq::Status(
        rodaq::StatusCode::Unavailable, std::string("dds"), -12,
        std::string("transport unavailable"));
}

}  // namespace

TEST(StatusTest, StatusCodeMatchesTaxonomyValues) {
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::Ok), 0U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::UnknownError), 10U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::InvalidArgument), 1U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::InvalidState), 2U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::NotFound), 3U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::Conflict), 4U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::ResourceExhausted), 5U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::Unavailable), 6U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::StorageError), 7U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::Timeout), 8U);
    EXPECT_EQ(static_cast<std::uint32_t>(rodaq::StatusCode::InternalError), 9U);
}

TEST(StatusTest, TwoArgumentConstructorUsesEmptyDomainAndZeroErrorCode) {
    const rodaq::Status error(rodaq::StatusCode::NotFound, "stream does not exist");

    EXPECT_EQ(error.status_code(), rodaq::StatusCode::NotFound);
    EXPECT_EQ(error.error_code(), 0);
    EXPECT_TRUE(error.domain().empty());
    EXPECT_EQ(error.message(), "stream does not exist");
}

TEST(StatusTest, FullConstructorPreservesOwnedFields) {
    const auto error = make_status_with_temporary_strings();

    EXPECT_EQ(error.status_code(), rodaq::StatusCode::Unavailable);
    EXPECT_EQ(error.error_code(), -12);
    EXPECT_EQ(error.domain(), "dds");
    EXPECT_EQ(error.message(), "transport unavailable");
}

TEST(StatusTest, EmptyDiagnosticsArePreserved) {
    const rodaq::Status error(rodaq::StatusCode::InvalidArgument, "", 0, "");

    EXPECT_TRUE(error.domain().empty());
    EXPECT_TRUE(error.message().empty());
}

TEST(StatusTest, CopyAndMovePreserveValues) {
    const rodaq::Status original(rodaq::StatusCode::StorageError, "mcap", 27, "write failed");

    const rodaq::Status copy(original);
    EXPECT_EQ(copy.status_code(), original.status_code());
    EXPECT_EQ(copy.error_code(), original.error_code());
    EXPECT_EQ(copy.domain(), original.domain());
    EXPECT_EQ(copy.message(), original.message());

    rodaq::Status copy_source(original);
    rodaq::Status moved(std::move(copy_source));
    EXPECT_EQ(moved.status_code(), original.status_code());
    EXPECT_EQ(moved.error_code(), original.error_code());
    EXPECT_EQ(moved.domain(), original.domain());
    EXPECT_EQ(moved.message(), original.message());
}

TEST(StatusTest, SuccessClearsDiagnosticsAndUnknownCodesArePreservedAsFailure) {
    const rodaq::Status success(rodaq::StatusCode::Ok, "backend", -17, "ignored");
    EXPECT_TRUE(success.ok());
    EXPECT_TRUE(success.domain().empty());
    EXPECT_EQ(success.error_code(), 0);
    EXPECT_TRUE(success.message().empty());

    const rodaq::Status failure(static_cast<rodaq::StatusCode>(999), "backend", -17, "failed");
    EXPECT_FALSE(failure.ok());
    EXPECT_EQ(failure.status_code(), rodaq::StatusCode::UnknownError);
    EXPECT_EQ(failure.domain(), "backend");
    EXPECT_EQ(failure.error_code(), -17);
    EXPECT_EQ(failure.message(), "failed");
    rodaq::Status copy(failure);
    rodaq::Status moved(std::move(copy));
    EXPECT_EQ(moved.status_code(), rodaq::StatusCode::UnknownError);
    EXPECT_EQ(moved.domain(), "backend");
    EXPECT_EQ(moved.error_code(), -17);
    EXPECT_EQ(moved.message(), "failed");
}
