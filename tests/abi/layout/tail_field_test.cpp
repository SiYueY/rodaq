#include <rodaq/plugin/abi/runtime.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#define RODAQ_CHECK_TAIL_FIELD(type, field, minimum_macro)                       \
    do {                                                                         \
        type value{};                                                            \
        value.struct_size = static_cast<std::uint32_t>(minimum_macro);           \
        EXPECT_TRUE(RODAQ_HAS_FIELD(type, &value, field));                       \
        value.struct_size = static_cast<std::uint32_t>(sizeof(type));            \
        EXPECT_TRUE(RODAQ_HAS_FIELD(type, &value, field));                       \
        value.struct_size = static_cast<std::uint32_t>(minimum_macro) - 1U;      \
        EXPECT_FALSE(RODAQ_HAS_FIELD(type, &value, field));                      \
        value.struct_size = static_cast<std::uint32_t>(offsetof(type, field));   \
        EXPECT_FALSE(RODAQ_HAS_FIELD(type, &value, field));                      \
        EXPECT_FALSE(RODAQ_HAS_FIELD(type, static_cast<type*>(nullptr), field)); \
    } while (false)

TEST(TailFieldTest, GatingCoversAllV1AppendableMinimumPrefixes) {
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_plugin_descriptor, description, RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_service_registry_vtable, query, RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_runtime_common_v1, report_diagnostic, RODAQ_RUNTIME_COMMON_V1_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_source_registration, source_id, RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_clock_registration, origin, RODAQ_CLOCK_REGISTRATION_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_schema_registration, representation, RODAQ_SCHEMA_REGISTRATION_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_stream_registration, native_id, RODAQ_STREAM_REGISTRATION_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_stream_descriptor_registration, timestamp_key_count,
        RODAQ_STREAM_DESCRIPTOR_REGISTRATION_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_record_submission, payload, RODAQ_RECORD_SUBMISSION_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_acquisition_submit_result, payload_disposition,
        RODAQ_ACQUISITION_SUBMIT_RESULT_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_runtime_acquisition_v1, submit, RODAQ_RUNTIME_ACQUISITION_V1_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(rodaq_source_view, source_id, RODAQ_SOURCE_VIEW_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(rodaq_clock_view, origin, RODAQ_CLOCK_VIEW_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(rodaq_schema_view, representation, RODAQ_SCHEMA_VIEW_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_timestamp_key_set_view, key_count, RODAQ_TIMESTAMP_KEY_SET_VIEW_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(rodaq_stream_view, stream_id, RODAQ_STREAM_VIEW_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_stream_descriptor_view, transport_timestamp_key_count,
        RODAQ_STREAM_DESCRIPTOR_VIEW_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_runtime_canonical_v1, get_stream_descriptor,
        RODAQ_RUNTIME_CANONICAL_V1_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(rodaq_config_vtable, as_string, RODAQ_CONFIG_VTABLE_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_acquisition_control_v1, quiesce_plugin, RODAQ_ACQUISITION_CONTROL_V1_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_storage_capabilities, close_progress_timeout_ns,
        RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_descriptor_readiness_result, failure,
        RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_storage_control_v1, ensure_descriptor_ready,
        RODAQ_STORAGE_CONTROL_V1_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_acquisition_plugin, query_interface, RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_storage_plugin, query_interface, RODAQ_STORAGE_PLUGIN_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_storage_open_request, target_uri, RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_storage_close_request, disposition, RODAQ_STORAGE_CLOSE_REQUEST_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_storage_submission, payload, RODAQ_STORAGE_SUBMISSION_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_storage_submit_result, persistence_error,
        RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE);
    RODAQ_CHECK_TAIL_FIELD(
        rodaq_runtime_storage_v1, report_failure, RODAQ_RUNTIME_STORAGE_V1_MIN_STRUCT_SIZE);
}

#undef RODAQ_CHECK_TAIL_FIELD
