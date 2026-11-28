#include <rodaq/acquisition/interface.hpp>
#include <rodaq/plugin/authoring.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

namespace acq = rodaq::acquisition;
namespace plugin = rodaq::plugin;

acq::RecordAttempt make_record() {
    acq::RecordAttempt result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = 1;
    result.descriptor_id = 1;
    return result;
}

struct HostCallbacks {
    static rodaq_status_code_t prepare_descriptor(
        void* ctx, rodaq_stream_id_t stream_id, const rodaq_acquisition_descriptor_t* descriptor,
        rodaq_descriptor_id_t* descriptor_id, rodaq_error_t*) noexcept {
        auto& self = *reinterpret_cast<HostCallbacks*>(ctx);
        ++self.prepare_calls;
        if (self.fail_prepare || descriptor == nullptr || stream_id == 0) {
            return RODAQ_STATUS_UNAVAILABLE;
        }
        *descriptor_id = self.next_descriptor_id;
        return RODAQ_STATUS_OK;
    }

    static rodaq_record_disposition_t submit_record(
        void* ctx, const rodaq_acquisition_record_attempt_t* value) noexcept {
        auto& self = *reinterpret_cast<HostCallbacks*>(ctx);
        ++self.submit_calls;
        if (value->payload.size == 0) {
            self.copied.clear();
        } else {
            self.copied.assign(value->payload.data, value->payload.data + value->payload.size);
        }
        return self.disposition;
    }

    static void report_event(void* ctx, const rodaq_acquisition_event_t* event) noexcept {
        auto& self = *reinterpret_cast<HostCallbacks*>(ctx);
        ++self.event_calls;
        self.last_event_kind = event->kind;
    }

    acq::CallbacksTable table() noexcept {
        return {
            {sizeof(acq::CallbacksTable)}, this, prepare_descriptor, submit_record, report_event};
    }

    bool fail_prepare{false};
    rodaq_descriptor_id_t next_descriptor_id{42};
    rodaq_record_disposition_t disposition{RODAQ_SUBMIT_ACCEPTED};
    std::uint32_t prepare_calls{0};
    std::uint32_t submit_calls{0};
    std::uint32_t event_calls{0};
    rodaq_acquisition_event_kind_t last_event_kind{RODAQ_ACQUISITION_EVENT_INVALID};
    std::vector<std::uint8_t> copied;
};

TEST(AcquisitionCallbacks, CopyCompletesBeforeCallbackReturnsAndRejectIsSuccessfulDisposition) {
    HostCallbacks host;
    acq::Callbacks cb(host.table());
    ASSERT_TRUE(static_cast<bool>(cb));

    std::array<std::uint8_t, 4> source{1, 2, 3, 4};
    auto value = make_record();
    value.payload = {source.data(), source.size()};
    EXPECT_EQ(cb.submit_record(value), RODAQ_SUBMIT_ACCEPTED);
    source.fill(9);
    EXPECT_EQ(host.copied, (std::vector<std::uint8_t>{1, 2, 3, 4}));

    host.disposition = RODAQ_SUBMIT_GATE_CLOSED;
    EXPECT_EQ(cb.submit_record(value), RODAQ_SUBMIT_GATE_CLOSED);

    host.disposition = 999;
    EXPECT_EQ(cb.submit_record(value), RODAQ_SUBMIT_INVALID);
}

TEST(AcquisitionCallbacks, PrepareDescriptorRejectsZeroIdAndReportsHostErrors) {
    HostCallbacks host;
    acq::Callbacks cb(host.table());

    acq::Descriptor descriptor{};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.stream_id = 1;
    descriptor.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    descriptor.channel_name = {"sample", 6};
    descriptor.message_encoding = {"raw", 3};

    auto bound = cb.prepare_descriptor(1, descriptor);
    ASSERT_TRUE(bound.ok());
    EXPECT_EQ(bound.value(), 42ULL);

    host.next_descriptor_id = 0;
    EXPECT_EQ(cb.prepare_descriptor(1, descriptor).status().code(), rodaq::StatusCode::Internal);

    host.fail_prepare = true;
    EXPECT_EQ(cb.prepare_descriptor(1, descriptor).status().code(), rodaq::StatusCode::Unavailable);
}

TEST(AcquisitionValidation, RecordReservedBytesAndAbsentFieldsFailClosed) {
    const auto original = make_record();
    ASSERT_TRUE(acq::abi::valid_record(&original));
    auto value = original;
    value.stream_id = 0;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.descriptor_id = 0;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.source_publish_time = 1;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value.has_source_publish_time = 1;
    EXPECT_TRUE(acq::abi::valid_record(&value));
    value = original;
    value.has_source_publish_time = 2;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.reserved[0] = 1;
    EXPECT_FALSE(acq::abi::valid_record(&value));
    value = original;
    value.header.struct_size = sizeof(value) - 1;
    EXPECT_FALSE(acq::abi::valid_record(&value));
}

TEST(AcquisitionValidation, EventScopeAndStatusVocabularyAreExplicit) {
    acq::AcquisitionEvent event{};
    event.header.struct_size = sizeof(event);
    event.kind = RODAQ_ACQUISITION_EVENT_TRANSPORT_READY;
    EXPECT_TRUE(acq::abi::valid_acquisition_event(&event));
    event.kind = RODAQ_ACQUISITION_EVENT_OVERFLOW;
    EXPECT_FALSE(acq::abi::valid_acquisition_event(&event));
    event.has_stream_id = 1;
    event.stream_id = 1;
    event.count = 3;
    EXPECT_TRUE(acq::abi::valid_acquisition_event(&event));
    event.status = RODAQ_STATUS_UNAVAILABLE;
    EXPECT_FALSE(acq::abi::valid_acquisition_event(&event));
    event.kind = RODAQ_ACQUISITION_EVENT_RECOVERABLE_FAILURE;
    event.count = 0;
    EXPECT_TRUE(acq::abi::valid_acquisition_event(&event));
    event.status = 99;
    EXPECT_FALSE(acq::abi::valid_acquisition_event(&event));
}

TEST(AcquisitionValidation, GenericSchemaSupportStaysWithBackendWhileRos2RequiresSchema) {
    acq::Descriptor descriptor{};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.stream_id = 1;
    descriptor.channel_name = {"sample", 6};
    descriptor.message_encoding = {"raw", 3};
    descriptor.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    EXPECT_TRUE(acq::abi::valid_descriptor(&descriptor));
    descriptor.data_profile = RODAQ_DATA_PROFILE_ROS2;
    descriptor.message_encoding = {"cdr", 3};
    EXPECT_FALSE(acq::abi::valid_descriptor(&descriptor));
    descriptor.schema_name = {"std_msgs/msg/Int32", 18};
    descriptor.schema_encoding = {"ros2msg", 7};
    const std::uint8_t schema[] = {'i', 'n', 't'};
    descriptor.schema = {schema, sizeof(schema)};
    EXPECT_TRUE(acq::abi::valid_descriptor(&descriptor));
    rodaq_ros2_qos_profile_t qos{};
    descriptor.ros2_offered_qos_profiles = &qos;
    descriptor.ros2_offered_qos_profile_count = 1;
    EXPECT_TRUE(acq::abi::valid_descriptor(&descriptor));
    qos.reliability = 99;
    EXPECT_FALSE(acq::abi::valid_descriptor(&descriptor));
    qos.reliability = RODAQ_ROS2_QOS_RELIABILITY_RELIABLE;
    qos.deadline.nsec = 1000000000;
    EXPECT_FALSE(acq::abi::valid_descriptor(&descriptor));
}

struct ControlBackend {
    rodaq::Result<void> prepare(
        const acq::PrepareConfig&, acq::Callbacks, plugin::Deadline budget) {
        deadline = budget.native().monotonic_deadline;
        return {};
    }

    rodaq::Result<void> start(plugin::Deadline budget) {
        deadline = budget.native().monotonic_deadline;
        if (throw_on_start) {
            throw std::runtime_error("boom");
        }
        return {};
    }

    rodaq::Result<void> quiesce(plugin::Deadline budget) {
        deadline = budget.native().monotonic_deadline;
        return {};
    }

    std::int64_t deadline{0};
    bool throw_on_start{false};
};

TEST(AcquisitionSdkControl, NativeAbsoluteBudgetIsPreservedAndNegativeBudgetRejected) {
    ControlBackend backend;
    auto* context = reinterpret_cast<rodaq_interface_context_t*>(&backend);
    const auto* table = acq::Provider<ControlBackend>::vtable();
    EXPECT_EQ(table->start(context, {12345}, nullptr), RODAQ_STATUS_OK);
    EXPECT_EQ(backend.deadline, 12345);
    EXPECT_EQ(table->quiesce(context, {-1}, nullptr), RODAQ_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(backend.deadline, 12345);
    backend.throw_on_start = true;
    EXPECT_EQ(table->start(context, {12345}, nullptr), RODAQ_STATUS_INTERNAL);
}

}  // namespace
