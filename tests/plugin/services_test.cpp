#include <rodaq/plugin/config_view.hpp>
#include <rodaq/plugin/services.hpp>
#include <rodaq/plugin/service_registry.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

template <typename T>
T take(rodaq::Result<T> result) {
    if (!result.ok()) {
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

rodaq_status_code lifecycle_snapshot(
    void*, rodaq_runtime_lifecycle_snapshot* output, rodaq_status*) noexcept {
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

const rodaq_runtime_common common_vtable = {
    RODAQ_RUNTIME_COMMON_PROTOCOL_VERSION,
    RODAQ_RUNTIME_COMMON_MIN_STRUCT_SIZE,
    &log_message,
    &monotonic_time,
    &lifecycle_snapshot,
    &report_diagnostic,
};

template <typename View>
rodaq_status_code complete_canonical_view(View* output) noexcept {
    ++counts.canonical;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    output->struct_size = sizeof(View);
    return RODAQ_STATUS_OK;
}

rodaq_status_code get_source(
    void*, rodaq_source_handle, rodaq_source_info* output, rodaq_status*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status_code get_clock(
    void*, rodaq_clock_handle, rodaq_clock_info* output, rodaq_status*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status_code get_schema(
    void*, rodaq_schema_handle, rodaq_schema_info* output, rodaq_status*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status_code get_timestamp_key_set(
    void*, std::uint64_t, rodaq_timestamp_key_set_info* output, rodaq_status*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status_code get_stream(
    void*, rodaq_stream_handle, rodaq_stream_info* output, rodaq_status*) noexcept {
    return complete_canonical_view(output);
}

rodaq_status_code get_stream_descriptor(
    void*, rodaq_stream_descriptor_handle, rodaq_stream_descriptor_info* output,
    rodaq_status*) noexcept {
    return complete_canonical_view(output);
}

const rodaq_runtime_canonical canonical_vtable = {
    RODAQ_RUNTIME_CANONICAL_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CANONICAL_MIN_STRUCT_SIZE,
    &get_source,
    &get_clock,
    &get_schema,
    &get_timestamp_key_set,
    &get_stream,
    &get_stream_descriptor,
};

rodaq_status_code register_projection(
    void*, const rodaq_projection_registration*, rodaq_status*) noexcept {
    ++counts.projection;
    return RODAQ_STATUS_OK;
}

rodaq_status_code get_projection(
    void*, rodaq_stream_descriptor_handle, rodaq_string_view, rodaq_projection_info* output,
    rodaq_status*) noexcept {
    ++counts.projection;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *output = {};
    output->struct_size = sizeof(*output);
    return RODAQ_STATUS_OK;
}

const rodaq_runtime_projection projection_vtable = {
    RODAQ_RUNTIME_PROJECTION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_PROJECTION_MIN_STRUCT_SIZE,
    &register_projection,
    &get_projection,
};

rodaq_status_code get_anchor(
    void*, rodaq_clock_handle, rodaq_unix_epoch_clock_anchor* output, rodaq_status*) noexcept {
    ++counts.clock;
    if (output == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *output = {};
    output->struct_size = sizeof(*output);
    return RODAQ_STATUS_OK;
}

const rodaq_runtime_clock_correlation clock_vtable = {
    RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CLOCK_CORRELATION_MIN_STRUCT_SIZE,
    &get_anchor,
};

rodaq_status_code register_source(
    void*, const rodaq_source_registration*, rodaq_source_handle* output, rodaq_status*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status_code register_clock(
    void*, const rodaq_clock_registration*, rodaq_clock_handle* output, rodaq_status*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status_code register_schema(
    void*, const rodaq_schema_registration*, rodaq_schema_handle* output, rodaq_status*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status_code register_stream(
    void*, const rodaq_stream_registration*, rodaq_stream_handle* output, rodaq_status*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status_code publish_descriptor(
    void*, const rodaq_stream_descriptor_registration*, rodaq_stream_descriptor_handle* output,
    rodaq_status*) noexcept {
    ++counts.acquisition;
    if (output != nullptr) {
        output->value = 1;
    }
    return output == nullptr ? RODAQ_STATUS_INVALID_ARGUMENT : RODAQ_STATUS_OK;
}

rodaq_status_code report_source(
    void*, rodaq_source_handle, rodaq_availability_state, const rodaq_status*,
    rodaq_status*) noexcept {
    ++counts.acquisition;
    return RODAQ_STATUS_OK;
}

rodaq_status_code report_stream(
    void*, rodaq_stream_handle, rodaq_availability_state, const rodaq_status*,
    rodaq_status*) noexcept {
    ++counts.acquisition;
    return RODAQ_STATUS_OK;
}

rodaq_status_code report_plugin(
    void*, rodaq_acquisition_plugin_state, const rodaq_status*, rodaq_status*) noexcept {
    ++counts.acquisition;
    return RODAQ_STATUS_OK;
}

rodaq_status_code submit_record(
    void*, const rodaq_record_submission*, rodaq_acquisition_submit_result* output,
    rodaq_status*) noexcept {
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

const rodaq_runtime_acquisition acquisition_vtable = {
    RODAQ_RUNTIME_ACQUISITION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_ACQUISITION_MIN_STRUCT_SIZE,
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
    void*, rodaq_storage_operation, rodaq_storage_persistence_state, const rodaq_status*) noexcept {
    ++counts.storage;
}

void storage_failure(void*, const rodaq_status*) noexcept { ++counts.storage; }

const rodaq_runtime_storage storage_vtable = {
    RODAQ_RUNTIME_STORAGE_PROTOCOL_VERSION,
    RODAQ_RUNTIME_STORAGE_MIN_STRUCT_SIZE,
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

TEST(PluginServicesTest, DefaultWrappersAreSafeAndResultApisReportInvalidState) {
    rodaq::plugin::CommonService common;
    rodaq::plugin::CanonicalService canonical;
    rodaq::plugin::ProjectionService projection;
    rodaq::plugin::ClockCorrelationService clock;
    rodaq::plugin::AcquisitionService acquisition;
    rodaq::plugin::StorageService storage;
    rodaq::plugin::ConfigView config;
    rodaq::plugin::ServiceRegistryRef registry;

    EXPECT_FALSE(common.valid());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, common.monotonic_time_ns().status().code());
    common.log(RODAQ_ABI_LOG_INFO, "category", "message");
    common.diagnostic(RODAQ_DIAGNOSTIC_INFO, "code", "message");
    EXPECT_EQ(rodaq::StatusCode::InvalidState, common.lifecycle_snapshot().status().code());

    EXPECT_FALSE(canonical.valid());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, canonical.get_source({}).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, canonical.get_clock({}).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, canonical.get_schema({}).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, canonical.get_timestamp_key_set(1).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, canonical.get_stream({}).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, canonical.get_stream_descriptor({}).status().code());

    EXPECT_FALSE(projection.valid());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        projection
            .register_entry(rodaq_stream_descriptor_handle{1}, "id", nullptr, 0)

            .status()
            .code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        projection.get(rodaq_stream_descriptor_handle{1}, "id").status().code());

    EXPECT_FALSE(clock.valid());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, clock.anchor(rodaq_clock_handle{1}).status().code());

    EXPECT_FALSE(acquisition.valid());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        acquisition.register_source(valid_source()).status().code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState, acquisition.register_clock(valid_clock()).status().code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        acquisition.register_schema(valid_schema()).status().code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        acquisition.register_stream(valid_stream()).status().code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        acquisition.publish_descriptor(valid_descriptor()).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, acquisition.submit(valid_record()).status().code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        acquisition
            .report_source(rodaq_source_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)

            .status()
            .code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        acquisition
            .report_stream(rodaq_stream_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)

            .status()
            .code());
    EXPECT_EQ(
        rodaq::StatusCode::InvalidState,
        acquisition
            .report_plugin_state(RODAQ_ACQUISITION_PLUGIN_AVAILABLE, nullptr)

            .status()
            .code());

    EXPECT_FALSE(storage.valid());
    storage.payload_consumed(rodaq_storage_operation{1});
    storage.persistence_completed(
        rodaq_storage_operation{1}, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED, nullptr);
    storage.report_failure(nullptr);

    EXPECT_FALSE(config.valid());
    EXPECT_EQ(rodaq::ConfigKind::Null, config.kind());
    EXPECT_EQ(0U, config.size());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, config.find("key").status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, config.at(0).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, config.key_at(0).status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, config.as_bool().status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, config.as_int64().status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, config.as_double().status().code());
    EXPECT_EQ(rodaq::StatusCode::InvalidState, config.as_string().status().code());

    EXPECT_EQ(rodaq::StatusCode::InvalidState, registry.query("service").status().code());
}

TEST(PluginServicesTest, ValidWrappersForwardEveryPublicOperation) {
    counts = {};
    int context = 0;
    auto common = take(rodaq::plugin::CommonService::from(
        rodaq::plugin::InterfaceRef{rodaq_interface{&context, &common_vtable}}));
    auto canonical = take(rodaq::plugin::CanonicalService::from(
        rodaq::plugin::InterfaceRef{rodaq_interface{&context, &canonical_vtable}}));
    auto projection = take(rodaq::plugin::ProjectionService::from(
        rodaq::plugin::InterfaceRef{rodaq_interface{&context, &projection_vtable}}));
    auto clock = take(rodaq::plugin::ClockCorrelationService::from(
        rodaq::plugin::InterfaceRef{rodaq_interface{&context, &clock_vtable}}));
    auto acquisition = take(rodaq::plugin::AcquisitionService::from(
        rodaq::plugin::InterfaceRef{rodaq_interface{&context, &acquisition_vtable}}));
    auto storage = take(rodaq::plugin::StorageService::from(
        rodaq::plugin::InterfaceRef{rodaq_interface{&context, &storage_vtable}}));

    common.log(RODAQ_ABI_LOG_INFO, "category", "message");
    const auto monotonic = common.monotonic_time_ns();
    ASSERT_TRUE(monotonic.ok());
    EXPECT_EQ(0U, monotonic.value());
    ASSERT_TRUE(common.lifecycle_snapshot().ok());
    common.diagnostic(RODAQ_DIAGNOSTIC_INFO, "code", "message");
    EXPECT_EQ(4, counts.common);

    ASSERT_TRUE(canonical.get_source(rodaq_source_handle{1}).ok());
    ASSERT_TRUE(canonical.get_clock(rodaq_clock_handle{1}).ok());
    ASSERT_TRUE(canonical.get_schema(rodaq_schema_handle{1}).ok());
    ASSERT_TRUE(canonical.get_timestamp_key_set(1).ok());
    ASSERT_TRUE(canonical.get_stream(rodaq_stream_handle{1}).ok());
    ASSERT_TRUE(canonical.get_stream_descriptor(rodaq_stream_descriptor_handle{1}).ok());
    EXPECT_EQ(6, counts.canonical);

    const std::uint8_t projection_payload[] = {1};
    ASSERT_TRUE(
        projection
            .register_entry(rodaq_stream_descriptor_handle{1}, "projection", projection_payload, 1)
            .ok());
    ASSERT_TRUE(projection.get(rodaq_stream_descriptor_handle{1}, "projection").ok());
    EXPECT_EQ(2, counts.projection);

    ASSERT_TRUE(clock.anchor(rodaq_clock_handle{1}).ok());
    EXPECT_EQ(1, counts.clock);

    ASSERT_TRUE(acquisition.register_source(valid_source()).ok());
    ASSERT_TRUE(acquisition.register_clock(valid_clock()).ok());
    ASSERT_TRUE(acquisition.register_schema(valid_schema()).ok());
    ASSERT_TRUE(acquisition.register_stream(valid_stream()).ok());
    ASSERT_TRUE(acquisition.publish_descriptor(valid_descriptor()).ok());
    ASSERT_TRUE(acquisition.submit(valid_record()).ok());
    ASSERT_TRUE(
        acquisition.report_source(rodaq_source_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
            .ok());
    ASSERT_TRUE(
        acquisition.report_stream(rodaq_stream_handle{1}, RODAQ_AVAILABILITY_AVAILABLE, nullptr)
            .ok());
    ASSERT_TRUE(acquisition.report_plugin_state(RODAQ_ACQUISITION_PLUGIN_AVAILABLE, nullptr).ok());
    EXPECT_EQ(9, counts.acquisition);

    storage.payload_consumed(rodaq_storage_operation{1});
    storage.persistence_completed(
        rodaq_storage_operation{1}, RODAQ_STORAGE_PERSISTENCE_SUCCEEDED, nullptr);
    storage.report_failure(nullptr);
    EXPECT_EQ(3, counts.storage);
}

TEST(PluginServicesTest, StructurallyInvalidAcquisitionInputsDoNotReachDAQ) {
    int context = 0;
    auto acquisition = take(rodaq::plugin::AcquisitionService::from(
        rodaq::plugin::InterfaceRef{rodaq_interface{&context, &acquisition_vtable}}));
    counts = {};

    auto source = valid_source();
    source.source_id.data = nullptr;
    source.source_id.size = 1;
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument, acquisition.register_source(source).status().code());

    auto clock = valid_clock();
    clock.resolution_denominator = 0;
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument, acquisition.register_clock(clock).status().code());

    auto schema = valid_schema();
    schema.type_identifier_present = 2;
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument, acquisition.register_schema(schema).status().code());

    auto stream = valid_stream();
    stream.native_id.size = 0;
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument, acquisition.register_stream(stream).status().code());

    auto descriptor = valid_descriptor();
    descriptor.timestamp_key_count = 1;
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        acquisition.publish_descriptor(descriptor).status().code());

    static const rodaq_timestamp_key timestamp_key[] = {
        {rodaq_string_view{"role", 4}, rodaq_clock_handle{1}}};
    descriptor = valid_descriptor();
    descriptor.timestamp_keys = timestamp_key;
    descriptor.timestamp_key_count = UINT32_MAX;
    EXPECT_EQ(
        rodaq::StatusCode::InvalidArgument,
        acquisition.publish_descriptor(descriptor).status().code());

    auto record = valid_record();
    record.timestamp_value_count = 1;
    EXPECT_EQ(rodaq::StatusCode::InvalidArgument, acquisition.submit(record).status().code());

    static const rodaq_timestamp_value timestamp_value[] = {{RODAQ_FALSE, 0, 0}};
    record = valid_record();
    record.timestamp_values = timestamp_value;
    record.timestamp_value_count = UINT32_MAX;
    EXPECT_EQ(rodaq::StatusCode::InvalidArgument, acquisition.submit(record).status().code());

    record = valid_record();
    record.payload.mode = RODAQ_PAYLOAD_ADOPT;
    EXPECT_EQ(rodaq::StatusCode::InvalidArgument, acquisition.submit(record).status().code());

    EXPECT_EQ(0, counts.acquisition);
}
