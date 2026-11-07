#include "canonical/canonical_state.hpp"

#include <rodaq/daq_config.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>

namespace rodaq {
namespace {

TEST(CanonicalAvailabilityTest, UpdatesSourceAndStreamOnlyForTheOwningPlugin) {
    DAQConfig config;
    std::pmr::monotonic_buffer_resource resource(1024U * 1024U);
    CanonicalState state(config, 1, &resource);
    const std::uint8_t source_id[] = {'s'};
    const std::uint8_t stream_id[] = {'t'};
    auto source = state.register_source(0, source_id, sizeof(source_id));
    auto stream = state.register_stream(0, stream_id, sizeof(stream_id));
    ASSERT_TRUE(source.ok());
    ASSERT_TRUE(stream.ok());

    ASSERT_TRUE(
        state.report_source_availability(0, source.value(), RODAQ_AVAILABILITY_UNAVAILABLE).ok());
    ASSERT_TRUE(
        state.report_stream_availability(0, stream.value(), RODAQ_AVAILABILITY_UNAVAILABLE).ok());
    const auto* source_entry = state.sources().find(source.value());
    const auto* stream_entry = state.streams().find(stream.value());
    ASSERT_NE(nullptr, source_entry);
    ASSERT_NE(nullptr, stream_entry);
    EXPECT_EQ(RODAQ_AVAILABILITY_UNAVAILABLE, source_entry->availability->load());
    EXPECT_EQ(RODAQ_AVAILABILITY_UNAVAILABLE, stream_entry->availability->load());

    const auto wrong_source_owner =
        state.report_source_availability(1, source.value(), RODAQ_AVAILABILITY_AVAILABLE);
    const auto wrong_stream_owner =
        state.report_stream_availability(1, stream.value(), RODAQ_AVAILABILITY_AVAILABLE);
    ASSERT_FALSE(wrong_source_owner.ok());
    ASSERT_FALSE(wrong_stream_owner.ok());
    EXPECT_EQ(StatusCode::InvalidState, wrong_source_owner.status().code());
    EXPECT_EQ(StatusCode::InvalidState, wrong_stream_owner.status().code());
}

TEST(CanonicalAvailabilityTest, RejectsUnknownHandlesAndInvalidStates) {
    DAQConfig config;
    std::pmr::monotonic_buffer_resource resource(1024U * 1024U);
    CanonicalState state(config, 1, &resource);

    const auto unknown = state.report_source_availability(
        0, SourceHandle::from_raw(42), RODAQ_AVAILABILITY_AVAILABLE);
    ASSERT_FALSE(unknown.ok());
    EXPECT_EQ(StatusCode::NotFound, unknown.status().code());

    const std::uint8_t stream_id[] = {'t'};
    auto stream = state.register_stream(0, stream_id, sizeof(stream_id));
    ASSERT_TRUE(stream.ok());
    const auto invalid = state.report_stream_availability(
        0, stream.value(), static_cast<rodaq_availability_state>(99));
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(StatusCode::InvalidArgument, invalid.status().code());
}

}  // namespace
}  // namespace rodaq
