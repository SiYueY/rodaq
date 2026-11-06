#include "canonical/canonical_state.hpp"

#include <rodaq/daq_config.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace rodaq {
namespace {

rodaq_clock_registration clock_registration(std::string_view native_id, std::uint64_t numerator) {
    return rodaq_clock_registration{
        sizeof(rodaq_clock_registration),
        0,
        rodaq_bytes_view{reinterpret_cast<const std::uint8_t*>(native_id.data()), native_id.size()},
        numerator,
        1,
        rodaq_string_view{"rodaq/unix_epoch", 16},
    };
}

rodaq_schema_registration schema_registration(std::string_view format) {
    return rodaq_schema_registration{
        sizeof(rodaq_schema_registration),
        RODAQ_FALSE,
        0,
        rodaq_string_view{format.data(), format.size()},
        {},
        {},
    };
}

TEST(CanonicalRegistrationTest, InterningIsScopedByOwnerAndPreservesIdentity) {
    DAQConfig config;
    std::pmr::monotonic_buffer_resource resource(1024U * 1024U);
    CanonicalState state(config, 1, &resource);
    const std::uint8_t source_id[] = {'s', 'o', 'u', 'r', 'c', 'e'};

    auto first = state.register_source(0, source_id, sizeof(source_id));
    auto duplicate = state.register_source(0, source_id, sizeof(source_id));
    auto foreign = state.register_source(1, source_id, sizeof(source_id));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(duplicate.ok());
    ASSERT_FALSE(foreign.ok());
    EXPECT_EQ(first.value(), duplicate.value());
    EXPECT_EQ(StatusCode::Conflict, foreign.status().status_code());

    rodaq_source_info info{};
    ASSERT_TRUE(state.get_source(first.value(), info).ok());
    EXPECT_EQ(6U, info.source_id.size);
    EXPECT_EQ(
        0, std::string_view(reinterpret_cast<const char*>(info.source_id.data), info.source_id.size)
               .compare("source"));
}

TEST(CanonicalRegistrationTest, ClockConflictConsumesNoDuplicateIdentity) {
    DAQConfig config;
    std::pmr::monotonic_buffer_resource resource(1024U * 1024U);
    CanonicalState state(config, 1, &resource);
    const auto registration = clock_registration("clock", 1);

    auto first = state.register_clock(0, registration);
    auto duplicate = state.register_clock(0, registration);
    auto conflict = state.register_clock(0, clock_registration("clock", 2));
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(duplicate.ok());
    ASSERT_FALSE(conflict.ok());
    EXPECT_EQ(first.value(), duplicate.value());
    EXPECT_EQ(StatusCode::Conflict, conflict.status().status_code());
}

TEST(CanonicalRegistrationTest, SchemaAndStreamDeduplicationRespectOwnershipScope) {
    DAQConfig config;
    std::pmr::monotonic_buffer_resource resource(1024U * 1024U);
    CanonicalState state(config, 3, &resource);

    auto first_schema = state.register_schema(schema_registration("cdr"));
    auto duplicate_schema = state.register_schema(schema_registration("cdr"));
    ASSERT_TRUE(first_schema.ok());
    ASSERT_TRUE(duplicate_schema.ok());
    EXPECT_EQ(first_schema.value(), duplicate_schema.value());

    const std::uint8_t stream_id[] = {'t', 'o', 'p', 'i', 'c'};
    auto first_stream = state.register_stream(0, stream_id, sizeof(stream_id));
    auto duplicate_stream = state.register_stream(0, stream_id, sizeof(stream_id));
    auto other_stream = state.register_stream(1, stream_id, sizeof(stream_id));
    ASSERT_TRUE(first_stream.ok());
    ASSERT_TRUE(duplicate_stream.ok());
    ASSERT_TRUE(other_stream.ok());
    EXPECT_EQ(first_stream.value(), duplicate_stream.value());
    EXPECT_NE(first_stream.value(), other_stream.value());
}

}  // namespace
}  // namespace rodaq
