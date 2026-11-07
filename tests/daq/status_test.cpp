#include <rodaq/status.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace {

rodaq::Status make_status_with_temporary_strings() {
    return rodaq::Status(
        rodaq::StatusCode::Unavailable, std::string("dds"), -12,
        std::string("transport unavailable"));
}

}  // namespace

TEST(StatusTest, StatusCodeMatchesTaxonomyValues) {
    static_assert(std::is_same_v<std::underlying_type_t<rodaq::StatusCode>, std::int32_t>);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::Ok), 0);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::UnknownError), 10);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::InvalidArgument), 1);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::InvalidState), 2);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::NotFound), 3);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::Conflict), 4);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::ResourceExhausted), 5);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::Unavailable), 6);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::StorageError), 7);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::Timeout), 8);
    EXPECT_EQ(static_cast<std::int32_t>(rodaq::StatusCode::InternalError), 9);
}

TEST(StatusTest, TwoArgumentConstructorUsesEmptyDomainAndZeroErrorCode) {
    const rodaq::Status error(rodaq::StatusCode::NotFound, "stream does not exist");

    EXPECT_EQ(error.code(), rodaq::StatusCode::NotFound);
    EXPECT_EQ(error.error_code(), 0);
    EXPECT_TRUE(error.domain().empty());
    EXPECT_EQ(error.message(), "stream does not exist");
}

TEST(StatusTest, FullConstructorPreservesOwnedFields) {
    const auto error = make_status_with_temporary_strings();

    EXPECT_EQ(error.code(), rodaq::StatusCode::Unavailable);
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
    EXPECT_EQ(copy.code(), original.code());
    EXPECT_EQ(copy.error_code(), original.error_code());
    EXPECT_EQ(copy.domain(), original.domain());
    EXPECT_EQ(copy.message(), original.message());

    rodaq::Status copy_source(original);
    rodaq::Status moved(std::move(copy_source));
    EXPECT_EQ(moved.code(), original.code());
    EXPECT_EQ(moved.error_code(), original.error_code());
    EXPECT_EQ(moved.domain(), original.domain());
    EXPECT_EQ(moved.message(), original.message());
}

TEST(StatusTest, SuccessClearsDiagnosticsAndUnknownCodesAreNotNormalized) {
    const rodaq::Status success = rodaq::Status::Ok();
    EXPECT_TRUE(success.ok());
    EXPECT_TRUE(success.domain().empty());
    EXPECT_EQ(success.error_code(), 0);
    EXPECT_TRUE(success.message().empty());

    const rodaq::Status failure(static_cast<rodaq::StatusCode>(999), "backend", -17, "failed");
    EXPECT_FALSE(failure.ok());
    EXPECT_EQ(failure.code(), static_cast<rodaq::StatusCode>(999));
    EXPECT_EQ(failure.domain(), "backend");
    EXPECT_EQ(failure.error_code(), -17);
    EXPECT_EQ(failure.message(), "failed");
    rodaq::Status copy(failure);
    rodaq::Status moved(std::move(copy));
    EXPECT_EQ(moved.code(), static_cast<rodaq::StatusCode>(999));
    EXPECT_EQ(moved.domain(), "backend");
    EXPECT_EQ(moved.error_code(), -17);
    EXPECT_EQ(moved.message(), "failed");

    const rodaq::Status two_argument_unknown(static_cast<rodaq::StatusCode>(999), "failed");
    EXPECT_FALSE(two_argument_unknown.ok());
    EXPECT_EQ(two_argument_unknown.code(), static_cast<rodaq::StatusCode>(999));
}

TEST(StatusContractTest, ConstructingOkWithDiagnosticsAborts) {
    EXPECT_DEATH(
        {
            const rodaq::Status invalid(rodaq::StatusCode::Ok, "ignored");
            static_cast<void>(invalid);
        },
        "");
    EXPECT_DEATH(
        {
            const rodaq::Status invalid(rodaq::StatusCode::Ok, "backend", -1, "ignored");
            static_cast<void>(invalid);
        },
        "");
}
