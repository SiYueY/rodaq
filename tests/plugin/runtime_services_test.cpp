#include <rodaq/plugin/config_view.hpp>
#include <rodaq/plugin/runtime_services.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.has_value()) {
        throw std::runtime_error("Expected successful wrapper construction");
    }
    return std::move(result.value());
}

struct CallbackCounts {
    int common = 0;
    int canonical = 0;
    int projection = 0;
    int clock = 0;
    int acquisition = 0;
    int storage = 0;
};

CallbackCounts counts;

void log_message(void*, rodaq_log_level, rodaq_string_view, rodaq_string_view) noexcept {
    ++counts.common;
}

std::uint64_t monotonic_time(void*) noexcept {
    ++counts.common;
    return 0;
}

rodaq_status lifecycle_snapshot(
    void*, rodaq_runtime_lifecycle_snapshot* output, rodaq_error*) noexcept {
    ++counts.common;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *output = {};
    output->state = RODAQ_RUNTIME_RUNNING;
    output->normal_acquisition_allowed = RODAQ_TRUE;
    output->generation = 9;
    return RODAQ_STATUS_OK;
}

void report_diagnostic(
    void*, rodaq_diagnostic_severity, rodaq_string_view, rodaq_string_view) noexcept {
    ++counts.common;
}

const rodaq_runtime_common_v1 common_vtable = {
    RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION,
    RODAQ_RUNTIME_COMMON_V1_MIN_STRUCT_SIZE,
    &log_message,
    &monotonic_time,
    &lifecycle_snapshot,
    &report_diagnostic,
};

template <typename View>
rodaq_status complete_canonical_view(View* output) noexcept {
    ++counts.canonical;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    output->struct_size = sizeof(View);
    return RODAQ_STATUS_OK;
}

rodaq_status get_source(
    void*, rodaq_source_handle, rodaq_source_view* output, rodaq_error*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status get_clock(void*, rodaq_clock_handle, rodaq_clock_view* output, rodaq_error*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status get_schema(
    void*, rodaq_schema_handle, rodaq_schema_view* output, rodaq_error*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status get_timestamp_key_set(
    void*, std::uint64_t, rodaq_timestamp_key_set_view* output, rodaq_error*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status get_stream(
    void*, rodaq_stream_handle, rodaq_stream_view* output, rodaq_error*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status get_stream_descriptor(
    void*, rodaq_stream_descriptor_handle, rodaq_stream_descriptor_view* output,
    rodaq_error*) noexcept {
    return complete_canonical_view(output);
}

const rodaq_runtime_canonical_v1 canonical_vtable = {
    RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CANONICAL_V1_MIN_STRUCT_SIZE,
    &get_source,
    &get_clock,
    &get_schema,
    &get_timestamp_key_set,
    &get_stream,
    &get_stream_descriptor,
};

rodaq_status register_projection(
    void*, const rodaq_projection_registration*, rodaq_error*) noexcept {
    ++counts.projection;
    return RODAQ_STATUS_OK;
}

rodaq_status get_projection(
    void*, rodaq_stream_descriptor_handle, rodaq_string_view, rodaq_projection_view* output,
    rodaq_error*) noexcept {
    ++counts.projection;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *output = {};
    output->struct_size = sizeof(*output);
    return RODAQ_STATUS_OK;
}

const rodaq_runtime_projection_v1 projection_vtable = {
    RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_PROJECTION_V1_MIN_STRUCT_SIZE,
    &register_projection,
    &get_projection,
};

rodaq_status get_anchor(
    void*, rodaq_clock_handle, rodaq_unix_epoch_clock_anchor* output, rodaq_error*) noexcept {
    ++counts.clock;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *output = {};
    output->struct_size = sizeof(*output);
    return RODAQ_STATUS_OK;
}

const rodaq_runtime_clock_correlation_v1 clock_vtable = {
    RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CLOCK_CORRELATION_V1_MIN_STRUCT_SIZE,
    &get_anchor,
};

rodaq_status register_source(
    void*, const rodaq_source_registration*, rodaq_source_handle* output, rodaq_error*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status register_clock(
    void*, const rodaq_clock_registration*, rodaq_clock_handle* output, rodaq_error*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status register_schema(
    void*, const rodaq_schema_registration*, rodaq_schema_handle* output, rodaq_error*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status register_stream(
    void*, const rodaq_stream_registration*, rodaq_stream_handle* output, rodaq_error*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status publish_descriptor(
    void*, const rodaq_stream_descriptor_registration*, rodaq_stream_descriptor_handle* output,
    rodaq_error*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status report_source(
    void*, rodaq_source_handle, rodaq_availability_state, const rodaq_error*,
    rodaq_error*) noexcept {
    ++counts.acquisition;
    return RODAQ_STATUS_OK;
}

rodaq_status report_stream(
    void*, rodaq_stream_handle, rodaq_availability_state, const rodaq_error*,
    rodaq_error*) noexcept {
    ++counts.acquisition;
    return RODAQ_STATUS_OK;
}

rodaq_status report_plugin(
    void*, rodaq_acquisition_plugin_state, const rodaq_error*, rodaq_error*) noexcept {
    ++counts.acquisition;
    return RODAQ_STATUS_OK;
}

rodaq_status submit_record(
    void*, const rodaq_record_submission*, rodaq_acquisition_submit_result* output,
    rodaq_error*) noexcept {
    ++counts.acquisition;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *output = {};
    output->struct_size = sizeof(*output);
    output->outcome = RODAQ_ADMISSION_ACCEPTED;
    output->payload_disposition = RODAQ_PAYLOAD_DISPOSITION_NOT_APPLICABLE;
    return RODAQ_STATUS_OK;
}

const rodaq_runtime_acquisition_v1 acquisition_vtable = {
    RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_ACQUISITION_V1_MIN_STRUCT_SIZE,
    &register_source,
    &register_clock,
    &register_schema,
    &register_stream,
    &publish_descriptor,
    &report_source,
    &report_stream,
    &report_plugin,
    &submit_record,
};

void payload_consumed(void*, rodaq_storage_operation) noexcept { ++counts.storage; }

void persistence_completed(
    void*, rodaq_storage_operation, rodaq_storage_persistence_state, const rodaq_error*) noexcept {
    ++counts.storage;
}

void storage_failure(void*, const rodaq_error*) noexcept { ++counts.storage; }

const rodaq_runtime_storage_v1 storage_vtable = {
    RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION,
    RODAQ_RUNTIME_STORAGE_V1_MIN_STRUCT_SIZE,
    &payload_consumed,
    &persistence_completed,
    &storage_failure,
};

rodaq_source_registration valid_source() {
    const std::uint8_t* source_id = reinterpret_cast<const std::uint8_t*>("source");
    return rodaq_source_registration{
        sizeof(rodaq_source_registration), 0, rodaq_bytes_view{source_id, 6}};
}

rodaq_clock_registration valid_clock() {
    const std::uint8_t* native_id = reinterpret_cast<const std::uint8_t*>("clock");
    return rodaq_clock_registration{
        sizeof(rodaq_clock_registration),
        0,
        rodaq_bytes_view{native_id, 5},
        1,
        1,
        rodaq_string_view{"rodaq/unix_epoch", sizeof("rodaq/unix_epoch") - 1},
    };
}

rodaq_schema_registration valid_schema() {
    return rodaq_schema_registration{
        sizeof(rodaq_schema_registration),
        RODAQ_FALSE,
        0,
        rodaq_string_view{"flatbuffer", sizeof("flatbuffer") - 1},
        {},
        {},
    };
}

rodaq_stream_registration valid_stream() {
    const std::uint8_t* native_id = reinterpret_cast<const std::uint8_t*>("stream");
    return rodaq_stream_registration{
        sizeof(rodaq_stream_registration), 0, rodaq_bytes_view{native_id, 6}};
}

rodaq_stream_descriptor_registration valid_descriptor() {
    return rodaq_stream_descriptor_registration{
        sizeof(rodaq_stream_descriptor_registration),
        RODAQ_FALSE,
        0,
        rodaq_stream_handle{1},
        rodaq_string_view{"topic", 5},
        {},
        rodaq_string_view{"cdr", 3},
        nullptr,
        0,
        0,
    };
}

rodaq_record_submission valid_record() {
    rodaq_record_submission submission{};
    submission.struct_size = sizeof(submission);
    submission.stream.value = 1;
    submission.descriptor.value = 2;
    submission.source_present = RODAQ_FALSE;
    submission.source_sequence_present = RODAQ_FALSE;
    submission.payload.mode = RODAQ_PAYLOAD_COPY;
    return submission;
}

}  // namespace

TEST(RuntimeServicesTest, DefaultWrappersAreSafeAndResultApisReportInvalidState) {
    rodaq::CommonRuntimeService common;
    rodaq::CanonicalRuntimeService canonical;
    rodaq::ProjectionRuntimeService projection;
    rodaq::ClockCorrelationRuntimeService clock;
    rodaq::AcquisitionRuntimeService acquisition;
    rodaq::StorageRuntimeService storage;
    rodaq::ConfigView config;
    rodaq::ServiceRegistryView registry;

    EXPECT_FALSE(common.Valid());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, common.MonotonicTimeNs().error().code());
    common.Log(RODAQ_LOG_INFO, "category", "message");
    common.Diagnostic(RODAQ_DIAGNOSTIC_INFO, "code", "message");
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, common.LifecycleSnapshot().error().code());

    EXPECT_FALSE(canonical.Valid());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, canonical.get_source({}).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, canonical.get_clock({}).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, canonical.get_schema({}).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, canonical.GetTimestampKeySet(1).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, canonical.get_stream({}).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, canonical.get_stream_descriptor({}).error().code());

    EXPECT_FALSE(projection.Valid());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState,
        projection.Register(rodaq_stream_descriptor_handle{1}, "id", nullptr, 0).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState,
        projection.Get(rodaq_stream_descriptor_handle{1}, "id").error().code());

    EXPECT_FALSE(clock.Valid());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, clock.Anchor(rodaq_clock_handle{1}).error().code());

    EXPECT_FALSE(acquisition.Valid());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState, acquisition.RegisterSource(valid_source()).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState, acquisition.RegisterClock(valid_clock()).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState, acquisition.RegisterSchema(valid_schema()).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState, acquisition.RegisterStream(valid_stream()).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState,
        acquisition.PublishDescriptor(valid_descriptor()).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, acquisition.Submit(valid_record()).error().code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState,
        acquisition.ReportSource(rodaq_source_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
            .error()
            .code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState,
        acquisition.ReportStream(rodaq_stream_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
            .error()
            .code());
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidState,
        acquisition.ReportPluginState(RODAQ_ACQUISITION_PLUGIN_AVAILABLE, nullptr).error().code());

    EXPECT_FALSE(storage.Valid());
    storage.PayloadConsumed(rodaq_storage_operation{1});
    storage.PersistenceCompleted(
        rodaq_storage_operation{1}, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED, nullptr);
    storage.ReportFailure(nullptr);

    EXPECT_FALSE(config.Valid());
    EXPECT_EQ(RODAQ_CONFIG_NULL, config.Kind());
    EXPECT_EQ(0U, config.Size());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, config.Find("key").error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, config.At(0).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, config.KeyAt(0).error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, config.AsBool().error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, config.AsInt64().error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, config.AsDouble().error().code());
    EXPECT_EQ(rodaq::ErrorCode::InvalidState, config.AsString().error().code());

    EXPECT_EQ(rodaq::ErrorCode::InvalidState, registry.Query("service").error().code());
}

TEST(RuntimeServicesTest, ValidWrappersForwardEveryPublicOperation) {
    counts = {};
    int context = 0;
    auto common = take(rodaq::CommonRuntimeService::From(
        rodaq::InterfaceView{rodaq_interface{&context, &common_vtable}}));
    auto canonical = take(rodaq::CanonicalRuntimeService::From(
        rodaq::InterfaceView{rodaq_interface{&context, &canonical_vtable}}));
    auto projection = take(rodaq::ProjectionRuntimeService::From(
        rodaq::InterfaceView{rodaq_interface{&context, &projection_vtable}}));
    auto clock = take(rodaq::ClockCorrelationRuntimeService::From(
        rodaq::InterfaceView{rodaq_interface{&context, &clock_vtable}}));
    auto acquisition = take(rodaq::AcquisitionRuntimeService::From(
        rodaq::InterfaceView{rodaq_interface{&context, &acquisition_vtable}}));
    auto storage = take(rodaq::StorageRuntimeService::From(
        rodaq::InterfaceView{rodaq_interface{&context, &storage_vtable}}));

    common.Log(RODAQ_LOG_INFO, "category", "message");
    const auto monotonic = common.MonotonicTimeNs();
    ASSERT_TRUE(monotonic.has_value());
    EXPECT_EQ(0U, monotonic.value());
    ASSERT_TRUE(common.LifecycleSnapshot().has_value());
    common.Diagnostic(RODAQ_DIAGNOSTIC_INFO, "code", "message");
    EXPECT_EQ(4, counts.common);

    ASSERT_TRUE(canonical.get_source(rodaq_source_handle{1}).has_value());
    ASSERT_TRUE(canonical.get_clock(rodaq_clock_handle{1}).has_value());
    ASSERT_TRUE(canonical.get_schema(rodaq_schema_handle{1}).has_value());
    ASSERT_TRUE(canonical.GetTimestampKeySet(1).has_value());
    ASSERT_TRUE(canonical.get_stream(rodaq_stream_handle{1}).has_value());
    ASSERT_TRUE(canonical.get_stream_descriptor(rodaq_stream_descriptor_handle{1}).has_value());
    EXPECT_EQ(6, counts.canonical);

    const std::uint8_t projection_payload[] = {1};
    ASSERT_TRUE(
        projection.Register(rodaq_stream_descriptor_handle{1}, "projection", projection_payload, 1)
            .has_value());
    ASSERT_TRUE(projection.Get(rodaq_stream_descriptor_handle{1}, "projection").has_value());
    EXPECT_EQ(2, counts.projection);

    ASSERT_TRUE(clock.Anchor(rodaq_clock_handle{1}).has_value());
    EXPECT_EQ(1, counts.clock);

    ASSERT_TRUE(acquisition.RegisterSource(valid_source()).has_value());
    ASSERT_TRUE(acquisition.RegisterClock(valid_clock()).has_value());
    ASSERT_TRUE(acquisition.RegisterSchema(valid_schema()).has_value());
    ASSERT_TRUE(acquisition.RegisterStream(valid_stream()).has_value());
    ASSERT_TRUE(acquisition.PublishDescriptor(valid_descriptor()).has_value());
    ASSERT_TRUE(acquisition.Submit(valid_record()).has_value());
    ASSERT_TRUE(
        acquisition.ReportSource(rodaq_source_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
            .has_value());
    ASSERT_TRUE(
        acquisition.ReportStream(rodaq_stream_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
            .has_value());
    ASSERT_TRUE(
        acquisition.ReportPluginState(RODAQ_ACQUISITION_PLUGIN_AVAILABLE, nullptr).has_value());
    EXPECT_EQ(9, counts.acquisition);

    storage.PayloadConsumed(rodaq_storage_operation{1});
    storage.PersistenceCompleted(
        rodaq_storage_operation{1}, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED, nullptr);
    storage.ReportFailure(nullptr);
    EXPECT_EQ(3, counts.storage);
}

TEST(RuntimeServicesTest, StructurallyInvalidAcquisitionInputsDoNotReachRuntime) {
    int context = 0;
    auto acquisition = take(rodaq::AcquisitionRuntimeService::From(
        rodaq::InterfaceView{rodaq_interface{&context, &acquisition_vtable}}));
    counts = {};

    auto source = valid_source();
    source.source_id.data = nullptr;
    source.source_id.size = 1;
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, acquisition.RegisterSource(source).error().code());

    auto clock = valid_clock();
    clock.resolution_denominator = 0;
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, acquisition.RegisterClock(clock).error().code());

    auto schema = valid_schema();
    schema.type_identifier_present = 2;
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, acquisition.RegisterSchema(schema).error().code());

    auto stream = valid_stream();
    stream.native_id.size = 0;
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, acquisition.RegisterStream(stream).error().code());

    auto descriptor = valid_descriptor();
    descriptor.timestamp_key_count = 1;
    EXPECT_EQ(
        rodaq::ErrorCode::InvalidArgument,
        acquisition.PublishDescriptor(descriptor).error().code());

    auto record = valid_record();
    record.timestamp_value_count = 1;
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, acquisition.Submit(record).error().code());

    record = valid_record();
    record.payload.mode = RODAQ_PAYLOAD_ADOPT;
    EXPECT_EQ(rodaq::ErrorCode::InvalidArgument, acquisition.Submit(record).error().code());

    EXPECT_EQ(0, counts.acquisition);
}
