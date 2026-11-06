#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/runtime_services.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string_view text(const char* data, std::uint32_t size) { return std::string_view(data, size); }

}  // namespace

TEST(PluginExceptionTest, KnownResultErrorMapsToStableStatusAndDiagnostic) {
    rodaq_status error{};
    const rodaq_status_code status = rodaq::plugin::invoke_void(
        []() -> rodaq::Result<void> {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "test", 17, "known failure");
        },
        &error);

    EXPECT_EQ(RODAQ_STATUS_UNAVAILABLE, status);
    EXPECT_EQ(17, error.error_code);
    EXPECT_EQ("test", text(error.domain, error.domain_size));
    EXPECT_EQ("known failure", text(error.message, error.message_size));
}

TEST(PluginExceptionTest, StandardExceptionBecomesInternalDiagnostic) {
    rodaq_status error{};
    const rodaq_status_code status = rodaq::plugin::invoke_void(
        []() -> rodaq::Result<void> { throw std::runtime_error("standard failure"); }, &error);

    EXPECT_EQ(RODAQ_STATUS_INTERNAL_ERROR, status);
    EXPECT_EQ("sdk", text(error.domain, error.domain_size));
    EXPECT_EQ("standard failure", text(error.message, error.message_size));
}

TEST(PluginExceptionTest, UnknownExceptionBecomesInternalDiagnostic) {
    rodaq_status error{};
    const rodaq_status_code status =
        rodaq::plugin::invoke_void([]() -> rodaq::Result<void> { throw 42; }, &error);

    EXPECT_EQ(RODAQ_STATUS_INTERNAL_ERROR, status);
    EXPECT_NE(std::string_view::npos, text(error.message, error.message_size).find("unknown"));
}

TEST(PluginExceptionTest, InterfaceTrampolineNeverReturnsOkWithNullInterface) {
    rodaq_interface interface {};
    rodaq_status error{};
    const rodaq_status_code status = rodaq::plugin::invoke_interface(
        []() -> rodaq::Result<rodaq::InterfaceView> { return rodaq::InterfaceView{}; }, &interface,
        &error);

    EXPECT_EQ(RODAQ_STATUS_INVALID_ARGUMENT, status);
    EXPECT_EQ(nullptr, interface.context);
    EXPECT_EQ(nullptr, interface.vtable);
}

TEST(PluginExceptionTest, DefaultRuntimeWrappersFailWithoutDereferencingNull) {
    rodaq::CommonRuntimeService common;
    rodaq::CanonicalRuntimeService canonical;
    rodaq::ProjectionRuntimeService projection;
    rodaq::ClockCorrelationRuntimeService clock;
    rodaq::AcquisitionRuntimeService acquisition;
    rodaq::StorageRuntimeService storage;

    EXPECT_FALSE(common.valid());
    EXPECT_FALSE(canonical.valid());
    EXPECT_FALSE(projection.valid());
    EXPECT_FALSE(clock.valid());
    EXPECT_FALSE(acquisition.valid());
    EXPECT_FALSE(storage.valid());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, common.lifecycle_snapshot().status().status_code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        canonical.get_stream(rodaq_stream_handle{}).status().status_code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        projection.get(rodaq_stream_descriptor_handle{}, "id").status().status_code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState, clock.anchor(rodaq_clock_handle{}).status().status_code());
}
