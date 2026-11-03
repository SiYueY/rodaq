#include "frozen_outputs.h"

#include "runtime/clock_correlation.hpp"
#include "runtime/interop_projection_registry.hpp"

#include <gtest/gtest.h>

#include <cstring>

TEST(FrozenV1OutputCompatibility, ProjectionAndClockRespectOldCallerCapacity) {
    rodaq::runtime::InteropProjectionRegistry registry({64, 4, 128, 256});
    ASSERT_TRUE(registry
                    .Register(
                        rodaq_stream_descriptor_handle{7}, "rodaq.test.projection.v1",
                        reinterpret_cast<const std::uint8_t*>("x"), 1)
                    .has_value());
    const auto interface = registry.AbiInterface();
    const auto* projection = static_cast<const rodaq_runtime_projection_v1*>(interface.vtable);

    struct ProjectionBuffer {
        frozen_projection_view_v1 value;
        std::uint8_t canary[32];
    } projection_buffer{};
    std::memset(projection_buffer.canary, 0xA5, sizeof(projection_buffer.canary));
    projection_buffer.value.struct_size = sizeof(projection_buffer.value);
    rodaq_error error{};
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        projection->get_projection(
            interface.context, rodaq_stream_descriptor_handle{7},
            rodaq_string_view{"rodaq.test.projection.v1", 24},
            reinterpret_cast<rodaq_projection_view*>(&projection_buffer.value), &error));
    for (std::uint8_t byte : projection_buffer.canary) {
        EXPECT_EQ(0xA5, byte);
    }

    auto clock = rodaq::runtime::ClockCorrelation::FromAnchor(
        rodaq::runtime::UnixEpochClockAnchor{rodaq_clock_handle{3}, 10, 1'000});
    ASSERT_TRUE(clock.has_value());
    auto correlation = std::move(clock.value());
    const auto clock_interface = correlation.AbiInterface();
    const auto* clock_table =
        static_cast<const rodaq_runtime_clock_correlation_v1*>(clock_interface.vtable);
    struct ClockBuffer {
        frozen_clock_anchor_v1 value;
        std::uint8_t canary[32];
    } clock_buffer{};
    std::memset(clock_buffer.canary, 0xA5, sizeof(clock_buffer.canary));
    clock_buffer.value.struct_size = sizeof(clock_buffer.value);
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        clock_table->get_unix_epoch_anchor(
            clock_interface.context, rodaq_clock_handle{3},
            reinterpret_cast<rodaq_unix_epoch_clock_anchor*>(&clock_buffer.value), &error));
    for (std::uint8_t byte : clock_buffer.canary) {
        EXPECT_EQ(0xA5, byte);
    }
}
