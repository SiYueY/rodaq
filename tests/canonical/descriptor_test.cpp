#include "canonical/canonical_state.hpp"

#include <rodaq/daq_config.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace rodaq {
namespace {

TEST(DescriptorTest, LookupReturnsPublishedMetadataAndRejectsStaleHandles) {
    DAQConfig config;
    std::pmr::monotonic_buffer_resource resource(1024U * 1024U);
    CanonicalState state(config, 4, &resource);
    const std::uint8_t stream_id[] = {'t'};
    auto stream = state.register_stream(0, stream_id, sizeof(stream_id));
    ASSERT_TRUE(stream.ok());

    std::pmr::vector<TimestampKeyRecord> keys(&resource);
    std::pmr::vector<rodaq_timestamp_key> abi_keys(&resource);
    keys.push_back(TimestampKeyRecord{
        ArenaString("sensor_time", &resource),
        ClockHandle::from_raw((std::uint64_t{4} << 32U) | 1U),
    });
    abi_keys.push_back(rodaq_timestamp_key{
        rodaq_string_view{"sensor_time", 11},
        rodaq_clock_handle{keys.back().clock.value()},
    });
    auto key_set = state.timestamp_key_sets().insert(TimestampKeySetEntry{
        state.allocate_timestamp_key_set_id(),
        std::move(keys),
        std::move(abi_keys),
    });
    ASSERT_TRUE(key_set.ok());
    state.timestamp_key_set_handles().push_back(key_set.value());
    const auto* key_set_entry = state.timestamp_key_sets().find(key_set.value());
    ASSERT_NE(nullptr, key_set_entry);

    auto descriptor = state.descriptors().insert(StreamDescriptorEntry{
        state.allocate_descriptor_id(),
        stream.value(),
        false,
        {},
        0,
        ArenaString("camera", &resource),
        ArenaString("rgb8", &resource),
        key_set_entry->timestamp_key_set_id,
        std::pmr::vector<rodaq_timestamp_key>(
            key_set_entry->abi_keys.begin(), key_set_entry->abi_keys.end(), &resource),
        std::pmr::vector<rodaq_timestamp_key>(
            key_set_entry->abi_keys.begin(), key_set_entry->abi_keys.end(), &resource),
        0,
    });
    ASSERT_TRUE(descriptor.ok());
    state.descriptor_handles().push_back(descriptor.value());

    rodaq_stream_descriptor_info info{};
    ASSERT_TRUE(state.get_stream_descriptor(descriptor.value(), info).ok());
    EXPECT_EQ("camera", std::string_view(info.name.data, info.name.size));
    EXPECT_EQ("rgb8", std::string_view(info.payload_encoding.data, info.payload_encoding.size));
    EXPECT_EQ(1U, info.transport_timestamp_key_count);
    EXPECT_EQ(key_set_entry->timestamp_key_set_id, info.timestamp_key_set_id);

    const auto stale = state.get_stream_descriptor(
        StreamDescriptorHandle::from_raw(descriptor.value().value() + 1U), info);
    ASSERT_FALSE(stale.ok());
    EXPECT_EQ(StatusCode::NotFound, stale.status().status_code());
}

}  // namespace
}  // namespace rodaq
