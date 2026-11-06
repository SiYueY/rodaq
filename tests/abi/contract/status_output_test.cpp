#include "baseline_outputs.h"

#include "rodaq/plugin/storage_plugin.hpp"

#include "clock/clock_correlation.hpp"
#include "projection/projection_registry.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#define RODAQ_ASSERT_BASELINE_PREFIX_FIELD(Baseline, Current, field)                        \
    static_assert(offsetof(Baseline, field) == offsetof(Current, field), #field " offset"); \
    static_assert(                                                                          \
        sizeof(std::declval<Baseline&>().field) == sizeof(std::declval<Current&>().field),  \
        #field " width")

static_assert(sizeof(baseline_projection_view) <= sizeof(rodaq_projection_info));
static_assert(alignof(baseline_projection_view) == alignof(rodaq_projection_info));
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(baseline_projection_view, rodaq_projection_info, struct_size);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(baseline_projection_view, rodaq_projection_info, reserved);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(baseline_projection_view, rodaq_projection_info, descriptor);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(baseline_projection_view, rodaq_projection_info, projection_id);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(baseline_projection_view, rodaq_projection_info, payload);

static_assert(sizeof(baseline_clock_anchor) <= sizeof(rodaq_unix_epoch_clock_anchor));
static_assert(alignof(baseline_clock_anchor) == alignof(rodaq_unix_epoch_clock_anchor));
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_clock_anchor, rodaq_unix_epoch_clock_anchor, struct_size);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(baseline_clock_anchor, rodaq_unix_epoch_clock_anchor, reserved);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(baseline_clock_anchor, rodaq_unix_epoch_clock_anchor, clock);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_clock_anchor, rodaq_unix_epoch_clock_anchor, clock_reading);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_clock_anchor, rodaq_unix_epoch_clock_anchor, unix_anchor_ns);

static_assert(sizeof(baseline_storage_capabilities) <= sizeof(rodaq_storage_capabilities));
static_assert(alignof(baseline_storage_capabilities) == alignof(rodaq_storage_capabilities));
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, struct_size);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, flags);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, full_fidelity);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, reserved);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, maximum_payload_bytes);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, maximum_inflight_records);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, maximum_inflight_bytes);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, handoff_progress_timeout_ns);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, descriptor_progress_timeout_ns);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, flush_progress_timeout_ns);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_capabilities, rodaq_storage_capabilities, close_progress_timeout_ns);

static_assert(
    sizeof(baseline_descriptor_readiness_result) <= sizeof(rodaq_descriptor_readiness_result));
static_assert(
    alignof(baseline_descriptor_readiness_result) == alignof(rodaq_descriptor_readiness_result));
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_descriptor_readiness_result, rodaq_descriptor_readiness_result, struct_size);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_descriptor_readiness_result, rodaq_descriptor_readiness_result, state);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_descriptor_readiness_result, rodaq_descriptor_readiness_result, failure);

static_assert(sizeof(baseline_storage_submit_result) <= sizeof(rodaq_storage_submit_result));
static_assert(alignof(baseline_storage_submit_result) == alignof(rodaq_storage_submit_result));
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_submit_result, rodaq_storage_submit_result, struct_size);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_submit_result, rodaq_storage_submit_result, reserved);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_submit_result, rodaq_storage_submit_result, payload_state);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_submit_result, rodaq_storage_submit_result, persistence_state);
RODAQ_ASSERT_BASELINE_PREFIX_FIELD(
    baseline_storage_submit_result, rodaq_storage_submit_result, persistence_error);

#undef RODAQ_ASSERT_BASELINE_PREFIX_FIELD

namespace {

constexpr std::uint8_t kCanary = 0xA5;

template <typename Struct>
constexpr std::uint32_t expected_initialized_prefix(std::uint32_t capacity) noexcept {
    return static_cast<std::uint32_t>(capacity < sizeof(Struct) ? capacity : sizeof(Struct));
}

const rodaq_plugin_descriptor status_descriptor = {
    RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE,
    0,
    {"rodaq.storage.status_output", sizeof("rodaq.storage.status_output") - 1},
    {"Output Compatibility", sizeof("Output Compatibility") - 1},
    {"1.0.0", sizeof("1.0.0") - 1},
    {"", 0},
};

class StatusStoragePlugin final : public rodaq::plugin::StoragePlugin {
public:
    rodaq::Result<void> bind(rodaq::plugin::StorageContext) override { return {}; }
    rodaq::Result<void> configure(const rodaq::plugin::ConfigView&) override { return {}; }
    rodaq::Result<void> open(const rodaq_storage_open_request&) override { return {}; }
    rodaq::Result<rodaq_storage_capabilities> get_capabilities() override {
        rodaq_storage_capabilities value{};
        value.struct_size = sizeof(value);
        value.flags = 7;
        value.full_fidelity = RODAQ_TRUE;
        value.maximum_payload_bytes = 11;
        value.maximum_inflight_records = 12;
        value.maximum_inflight_bytes = 13;
        value.handoff_progress_timeout_ns = 14;
        value.descriptor_progress_timeout_ns = 15;
        value.flush_progress_timeout_ns = 16;
        value.close_progress_timeout_ns = 17;
        return value;
    }
    rodaq::Result<rodaq_descriptor_readiness_result> ensure_descriptor_ready(
        rodaq_stream_descriptor_handle) override {
        rodaq_descriptor_readiness_result value{};
        value.struct_size = sizeof(value);
        value.state = RODAQ_DESCRIPTOR_READY;
        return value;
    }
    rodaq::Result<rodaq_storage_submit_result> submit(const rodaq_storage_submission&) override {
        rodaq_storage_submit_result value{};
        value.struct_size = sizeof(value);
        value.payload_state = RODAQ_STORAGE_PAYLOAD_CONSUMED;
        value.persistence_state = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
        return value;
    }
    rodaq::Result<void> flush() override { return {}; }
    rodaq::Result<void> close(rodaq_storage_close_disposition) override { return {}; }
};

struct StorageAbiInstance {
    const rodaq_storage_plugin* protocol = nullptr;
    void* instance = nullptr;

    StorageAbiInstance() = default;
    StorageAbiInstance(const StorageAbiInstance&) = delete;
    StorageAbiInstance& operator=(const StorageAbiInstance&) = delete;
    StorageAbiInstance(StorageAbiInstance&& other) noexcept
    : protocol(other.protocol), instance(std::exchange(other.instance, nullptr)) {}
    StorageAbiInstance& operator=(StorageAbiInstance&& other) noexcept {
        if (this != &other) {
            if (instance != nullptr) {
                protocol->destroy(instance);
            }
            protocol = other.protocol;
            instance = std::exchange(other.instance, nullptr);
        }
        return *this;
    }

    ~StorageAbiInstance() {
        if (instance != nullptr) {
            protocol->destroy(instance);
        }
    }
};

StorageAbiInstance create_storage_instance() {
    StorageAbiInstance owner;
    owner.protocol = rodaq_get_storage_plugin();
    EXPECT_NE(nullptr, owner.protocol);
    if (owner.protocol == nullptr) {
        return owner;
    }
    rodaq_status error{};
    EXPECT_EQ(RODAQ_STATUS_OK, owner.protocol->create(&owner.instance, &error));
    return owner;
}

void expect_canary(const std::uint8_t* bytes, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        EXPECT_EQ(kCanary, bytes[index]);
    }
}

void fill_canary(std::uint8_t* bytes, std::size_t size) { std::memset(bytes, kCanary, size); }

}  // namespace

RODAQ_EXPORT_STORAGE_PLUGIN(StatusStoragePlugin, status_descriptor)

TEST(BaselineOutputCompatibility, ProjectionAndClockRespectOldCallerCapacity) {
    rodaq::ProjectionRegistry registry({64, 4, 128, 256});
    ASSERT_TRUE(registry
                    .register_entry(
                        rodaq_stream_descriptor_handle{7}, "rodaq.test.projection",
                        reinterpret_cast<const std::uint8_t*>("x"), 1)
                    .ok());
    const auto interface = registry.abi_interface();
    const auto* projection = static_cast<const rodaq_runtime_projection*>(interface.vtable);

    struct ProjectionBuffer {
        baseline_projection_view value;
        std::uint8_t canary[32];
    } projection_buffer{};
    std::memset(projection_buffer.canary, 0xA5, sizeof(projection_buffer.canary));
    const std::uint32_t frozen_projection_capacity = sizeof(projection_buffer.value);
    projection_buffer.value.struct_size = frozen_projection_capacity;
    rodaq_status error{};
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        projection->get_projection(
            interface.context, rodaq_stream_descriptor_handle{7},
            rodaq_string_view{"rodaq.test.projection", 21},
            reinterpret_cast<rodaq_projection_info*>(&projection_buffer.value), &error));
    EXPECT_EQ(
        expected_initialized_prefix<rodaq_projection_info>(frozen_projection_capacity),
        projection_buffer.value.struct_size);
    EXPECT_EQ(0U, projection_buffer.value.reserved);
    expect_canary(projection_buffer.canary, sizeof(projection_buffer.canary));

    {
        struct {
            rodaq_projection_info value;
            std::uint8_t canary[32];
        } current{};
        fill_canary(current.canary, sizeof(current.canary));
        const std::uint32_t current_capacity = sizeof(current.value);
        current.value.struct_size = current_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            projection->get_projection(
                interface.context, rodaq_stream_descriptor_handle{7},
                rodaq_string_view{"rodaq.test.projection", 21}, &current.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_projection_info>(current_capacity),
            current.value.struct_size);
        expect_canary(current.canary, sizeof(current.canary));
    }

    {
        struct {
            std::uint32_t struct_size;
            std::uint8_t canary[16];
        } too_small{};
        fill_canary(too_small.canary, sizeof(too_small.canary));
        too_small.struct_size = RODAQ_PROJECTION_INFO_MIN_STRUCT_SIZE - 1;
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            projection->get_projection(
                interface.context, rodaq_stream_descriptor_handle{7},
                rodaq_string_view{"rodaq.test.projection", 21},
                reinterpret_cast<rodaq_projection_info*>(&too_small), &error));
        expect_canary(too_small.canary, sizeof(too_small.canary));
    }

    {
        struct {
            rodaq_projection_info value;
            std::uint8_t canary[32];
        } larger{};
        fill_canary(larger.canary, sizeof(larger.canary));
        const std::uint32_t larger_capacity = sizeof(larger.value) + sizeof(larger.canary);
        larger.value.struct_size = larger_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            projection->get_projection(
                interface.context, rodaq_stream_descriptor_handle{7},
                rodaq_string_view{"rodaq.test.projection", 21}, &larger.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_projection_info>(larger_capacity),
            larger.value.struct_size);
        expect_canary(larger.canary, sizeof(larger.canary));
    }

    auto clock = rodaq::ClockCorrelation::from_anchor(
        rodaq::UnixEpochClockAnchor{rodaq_clock_handle{3}, 10, 1'000});
    ASSERT_TRUE(clock.ok());
    auto correlation = std::move(clock.value());
    const auto clock_interface = correlation.abi_interface();
    const auto* clock_table =
        static_cast<const rodaq_runtime_clock_correlation*>(clock_interface.vtable);
    struct ClockBuffer {
        baseline_clock_anchor value;
        std::uint8_t canary[32];
    } clock_buffer{};
    std::memset(clock_buffer.canary, 0xA5, sizeof(clock_buffer.canary));
    const std::uint32_t frozen_clock_capacity = sizeof(clock_buffer.value);
    clock_buffer.value.struct_size = frozen_clock_capacity;
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        clock_table->get_unix_epoch_anchor(
            clock_interface.context, rodaq_clock_handle{3},
            reinterpret_cast<rodaq_unix_epoch_clock_anchor*>(&clock_buffer.value), &error));
    EXPECT_EQ(
        expected_initialized_prefix<rodaq_unix_epoch_clock_anchor>(frozen_clock_capacity),
        clock_buffer.value.struct_size);
    EXPECT_EQ(0U, clock_buffer.value.reserved);
    expect_canary(clock_buffer.canary, sizeof(clock_buffer.canary));

    {
        struct {
            rodaq_unix_epoch_clock_anchor value;
            std::uint8_t canary[32];
        } current{};
        fill_canary(current.canary, sizeof(current.canary));
        const std::uint32_t current_capacity = sizeof(current.value);
        current.value.struct_size = current_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            clock_table->get_unix_epoch_anchor(
                clock_interface.context, rodaq_clock_handle{3}, &current.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_unix_epoch_clock_anchor>(current_capacity),
            current.value.struct_size);
        expect_canary(current.canary, sizeof(current.canary));
    }

    {
        struct {
            std::uint32_t struct_size;
            std::uint8_t canary[16];
        } too_small{};
        fill_canary(too_small.canary, sizeof(too_small.canary));
        too_small.struct_size = RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE - 1;
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            clock_table->get_unix_epoch_anchor(
                clock_interface.context, rodaq_clock_handle{3},
                reinterpret_cast<rodaq_unix_epoch_clock_anchor*>(&too_small), &error));
        expect_canary(too_small.canary, sizeof(too_small.canary));
    }

    {
        struct {
            rodaq_unix_epoch_clock_anchor value;
            std::uint8_t canary[32];
        } larger{};
        fill_canary(larger.canary, sizeof(larger.canary));
        const std::uint32_t larger_capacity = sizeof(larger.value) + sizeof(larger.canary);
        larger.value.struct_size = larger_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            clock_table->get_unix_epoch_anchor(
                clock_interface.context, rodaq_clock_handle{3}, &larger.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_unix_epoch_clock_anchor>(larger_capacity),
            larger.value.struct_size);
        expect_canary(larger.canary, sizeof(larger.canary));
    }
}

TEST(BaselineOutputCompatibility, StorageCapabilitiesRespectCallerCapacity) {
    rodaq_status error{};
    StorageAbiInstance owner = create_storage_instance();
    ASSERT_NE(nullptr, owner.instance);
    rodaq_interface interface {};
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        owner.protocol->query_interface(
            owner.instance,
            rodaq_string_view{
                RODAQ_STORAGE_CONTROL_INTERFACE_ID, sizeof(RODAQ_STORAGE_CONTROL_INTERFACE_ID) - 1},
            &interface, &error));
    const auto* control = static_cast<const rodaq_storage_control*>(interface.vtable);
    ASSERT_NE(nullptr, control);

    {
        struct {
            baseline_storage_capabilities value;
            std::uint8_t canary[16];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t frozen_capacity = sizeof(buffer.value);
        buffer.value.struct_size = frozen_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            control->get_capabilities(
                interface.context, reinterpret_cast<rodaq_storage_capabilities*>(&buffer.value),
                &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_storage_capabilities>(frozen_capacity),
            buffer.value.struct_size);
        EXPECT_EQ(0U, buffer.value.flags);
        EXPECT_EQ(0U, buffer.value.reserved);
        EXPECT_EQ(17U, buffer.value.close_progress_timeout_ns);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            rodaq_storage_capabilities value;
            std::uint8_t canary[32];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t current_capacity = sizeof(buffer.value);
        buffer.value.struct_size = current_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK, control->get_capabilities(interface.context, &buffer.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_storage_capabilities>(current_capacity),
            buffer.value.struct_size);
        EXPECT_EQ(0U, buffer.value.flags);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            std::uint32_t struct_size;
            std::uint8_t canary[16];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        buffer.struct_size = RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE - 1;
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            control->get_capabilities(
                interface.context, reinterpret_cast<rodaq_storage_capabilities*>(&buffer), &error));
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            rodaq_storage_capabilities value;
            std::uint8_t canary[32];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t larger_capacity = sizeof(buffer.value) + sizeof(buffer.canary);
        buffer.value.struct_size = larger_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK, control->get_capabilities(interface.context, &buffer.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_storage_capabilities>(larger_capacity),
            buffer.value.struct_size);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }
}

TEST(BaselineOutputCompatibility, DescriptorReadinessRespectsCallerCapacity) {
    rodaq_status error{};
    StorageAbiInstance owner = create_storage_instance();
    ASSERT_NE(nullptr, owner.instance);
    rodaq_interface interface {};
    ASSERT_EQ(
        RODAQ_STATUS_OK,
        owner.protocol->query_interface(
            owner.instance,
            rodaq_string_view{
                RODAQ_STORAGE_CONTROL_INTERFACE_ID, sizeof(RODAQ_STORAGE_CONTROL_INTERFACE_ID) - 1},
            &interface, &error));
    const auto* control = static_cast<const rodaq_storage_control*>(interface.vtable);
    ASSERT_NE(nullptr, control);

    {
        struct {
            baseline_descriptor_readiness_result value;
            std::uint8_t canary[16];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t frozen_capacity = sizeof(buffer.value);
        buffer.value.struct_size = frozen_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            control->ensure_descriptor_ready(
                interface.context, rodaq_stream_descriptor_handle{42},
                reinterpret_cast<rodaq_descriptor_readiness_result*>(&buffer.value), &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_descriptor_readiness_result>(frozen_capacity),
            buffer.value.struct_size);
        EXPECT_EQ(RODAQ_DESCRIPTOR_READY, buffer.value.state);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            rodaq_descriptor_readiness_result value;
            std::uint8_t canary[32];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t current_capacity = sizeof(buffer.value);
        buffer.value.struct_size = current_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            control->ensure_descriptor_ready(
                interface.context, rodaq_stream_descriptor_handle{42}, &buffer.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_descriptor_readiness_result>(current_capacity),
            buffer.value.struct_size);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            std::uint32_t struct_size;
            std::uint8_t canary[16];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        buffer.struct_size = RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE - 1;
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            control->ensure_descriptor_ready(
                interface.context, rodaq_stream_descriptor_handle{42},
                reinterpret_cast<rodaq_descriptor_readiness_result*>(&buffer), &error));
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            rodaq_descriptor_readiness_result value;
            std::uint8_t canary[32];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t larger_capacity = sizeof(buffer.value) + sizeof(buffer.canary);
        buffer.value.struct_size = larger_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            control->ensure_descriptor_ready(
                interface.context, rodaq_stream_descriptor_handle{42}, &buffer.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_descriptor_readiness_result>(larger_capacity),
            buffer.value.struct_size);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }
}

TEST(BaselineOutputCompatibility, StorageSubmitRespectsCallerCapacity) {
    rodaq_status error{};
    StorageAbiInstance owner = create_storage_instance();
    ASSERT_NE(nullptr, owner.instance);
    rodaq_storage_open_request open{};
    open.struct_size = sizeof(open);
    const char session[] = "session";
    const char target[] = "memory://status";
    open.session_id = rodaq_string_view{session, sizeof(session) - 1};
    open.target_uri = rodaq_string_view{target, sizeof(target) - 1};
    ASSERT_EQ(RODAQ_STATUS_OK, owner.protocol->open(owner.instance, &open, &error));

    rodaq_storage_submission submission{};
    submission.struct_size = sizeof(submission);
    submission.operation.value = 1;
    submission.stream.value = 2;
    submission.descriptor.value = 3;
    submission.source_present = RODAQ_FALSE;
    submission.source_sequence_present = RODAQ_FALSE;

    {
        struct {
            baseline_storage_submit_result value;
            std::uint8_t canary[16];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t frozen_capacity = sizeof(buffer.value);
        buffer.value.struct_size = frozen_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            owner.protocol->submit(
                owner.instance, &submission,
                reinterpret_cast<rodaq_storage_submit_result*>(&buffer.value), &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_storage_submit_result>(frozen_capacity),
            buffer.value.struct_size);
        EXPECT_EQ(0U, buffer.value.reserved);
        EXPECT_EQ(RODAQ_STORAGE_PAYLOAD_CONSUMED, buffer.value.payload_state);
        EXPECT_EQ(RODAQ_STORAGE_PERSISTENCE_SUCCEEDED, buffer.value.persistence_state);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            rodaq_storage_submit_result value;
            std::uint8_t canary[32];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t current_capacity = sizeof(buffer.value);
        buffer.value.struct_size = current_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            owner.protocol->submit(owner.instance, &submission, &buffer.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_storage_submit_result>(current_capacity),
            buffer.value.struct_size);
        EXPECT_EQ(0U, buffer.value.reserved);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            std::uint32_t struct_size;
            std::uint8_t canary[16];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        buffer.struct_size = RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE - 1;
        EXPECT_EQ(
            RODAQ_STATUS_INVALID_ARGUMENT,
            owner.protocol->submit(
                owner.instance, &submission,
                reinterpret_cast<rodaq_storage_submit_result*>(&buffer), &error));
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }

    {
        struct {
            rodaq_storage_submit_result value;
            std::uint8_t canary[32];
        } buffer{};
        fill_canary(buffer.canary, sizeof(buffer.canary));
        const std::uint32_t larger_capacity = sizeof(buffer.value) + sizeof(buffer.canary);
        buffer.value.struct_size = larger_capacity;
        ASSERT_EQ(
            RODAQ_STATUS_OK,
            owner.protocol->submit(owner.instance, &submission, &buffer.value, &error));
        EXPECT_EQ(
            expected_initialized_prefix<rodaq_storage_submit_result>(larger_capacity),
            buffer.value.struct_size);
        expect_canary(buffer.canary, sizeof(buffer.canary));
    }
}
