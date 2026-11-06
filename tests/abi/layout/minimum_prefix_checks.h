#ifndef RODAQ_TESTS_ABI_MINIMUM_PREFIX_CHECKS_H
#define RODAQ_TESTS_ABI_MINIMUM_PREFIX_CHECKS_H

#include <rodaq/plugin/abi/runtime.h>

#if defined(__cplusplus)
#define RODAQ_TEST_STATIC_ASSERT static_assert
#else
#define RODAQ_TEST_STATIC_ASSERT _Static_assert
#endif

#define RODAQ_EXPECT_MINIMUM_PREFIX(type, field, expected_offset, expected_minimum, minimum_macro) \
    RODAQ_TEST_STATIC_ASSERT(                                                                      \
        offsetof(type, field) == expected_offset, #type " minimum field offset");                  \
    RODAQ_TEST_STATIC_ASSERT(minimum_macro == expected_minimum, #type " minimum prefix")

RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_plugin_descriptor, struct_size) == 0, "descriptor size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_source_registration, struct_size) == 0, "source registration size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_clock_registration, struct_size) == 0, "clock registration size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_schema_registration, struct_size) == 0, "schema registration size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_stream_registration, struct_size) == 0, "stream registration size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_stream_descriptor_registration, struct_size) == 0,
    "descriptor registration size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_record_submission, struct_size) == 0, "record submission size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_acquisition_submit_result, struct_size) == 0, "acquisition result size slot");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_source_view, struct_size) == 0, "source view size slot");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_clock_view, struct_size) == 0, "clock view size slot");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_schema_view, struct_size) == 0, "schema view size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_timestamp_key_set_view, struct_size) == 0, "timestamp view size slot");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_stream_view, struct_size) == 0, "stream view size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_stream_descriptor_view, struct_size) == 0, "descriptor view size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_storage_capabilities, struct_size) == 0, "capabilities size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_descriptor_readiness_result, struct_size) == 0, "readiness size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_storage_open_request, struct_size) == 0, "open request size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_storage_close_request, struct_size) == 0, "close request size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_storage_submission, struct_size) == 0, "storage submission size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_storage_submit_result, struct_size) == 0, "storage result size slot");

RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_service_registry_vtable, struct_size) == 4, "registry vtable size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_common, struct_size) == 4, "common vtable size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_acquisition, struct_size) == 4, "acquisition vtable size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_canonical, struct_size) == 4, "canonical vtable size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_projection, struct_size) == 4, "projection vtable size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_clock_correlation, struct_size) == 4,
    "clock correlation vtable size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_config_vtable, struct_size) == 4, "config vtable size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_acquisition_control, struct_size) == 4, "acquisition control size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_storage_control, struct_size) == 4, "storage control size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_acquisition_plugin, struct_size) == 4, "acquisition plugin size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_storage_plugin, struct_size) == 4, "storage plugin size slot");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_storage, struct_size) == 4, "storage vtable size slot");

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_plugin_descriptor, description, 56, 72, RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_service_registry_vtable, query, 8, 16, RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_runtime_common, report_diagnostic, 32, 40, RODAQ_RUNTIME_COMMON_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_source_registration, source_id, 8, 24, RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_clock_registration, origin, 40, 56, RODAQ_CLOCK_REGISTRATION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_schema_registration, representation, 48, 64, RODAQ_SCHEMA_REGISTRATION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_stream_registration, native_id, 8, 24, RODAQ_STREAM_REGISTRATION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_stream_descriptor_registration, timestamp_key_count, 72, 76,
    RODAQ_STREAM_DESCRIPTOR_REGISTRATION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_record_submission, payload, 64, 104, RODAQ_RECORD_SUBMISSION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_acquisition_submit_result, payload_disposition, 12, 16,
    RODAQ_ACQUISITION_SUBMIT_RESULT_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_runtime_acquisition, submit, 72, 80, RODAQ_RUNTIME_ACQUISITION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_source_view, source_id, 16, 32, RODAQ_SOURCE_VIEW_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(rodaq_clock_view, origin, 40, 56, RODAQ_CLOCK_VIEW_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_schema_view, representation, 64, 80, RODAQ_SCHEMA_VIEW_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_timestamp_key_set_view, key_count, 24, 28, RODAQ_TIMESTAMP_KEY_SET_VIEW_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_stream_view, stream_id, 16, 24, RODAQ_STREAM_VIEW_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_stream_descriptor_view, transport_timestamp_key_count, 112, 116,
    RODAQ_STREAM_DESCRIPTOR_VIEW_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_runtime_canonical, get_stream_descriptor, 48, 56,
    RODAQ_RUNTIME_CANONICAL_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_projection_registration, payload, 32, 48, RODAQ_PROJECTION_REGISTRATION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_projection_view, payload, 32, 48, RODAQ_PROJECTION_VIEW_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_runtime_projection, get_projection, 16, 24, RODAQ_RUNTIME_PROJECTION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_unix_epoch_clock_anchor, unix_anchor_ns, 24, 32,
    RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_runtime_clock_correlation, get_unix_epoch_anchor, 8, 16,
    RODAQ_RUNTIME_CLOCK_CORRELATION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_config_vtable, as_string, 72, 80, RODAQ_CONFIG_VTABLE_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_acquisition_control, quiesce_plugin, 40, 48, RODAQ_ACQUISITION_CONTROL_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_storage_capabilities, close_progress_timeout_ns, 64, 72,
    RODAQ_STORAGE_CAPABILITIES_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_descriptor_readiness_result, failure, 8, 336,
    RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_storage_control, ensure_descriptor_ready, 16, 24, RODAQ_STORAGE_CONTROL_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_acquisition_plugin, query_interface, 64, 72, RODAQ_ACQUISITION_PLUGIN_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_storage_plugin, query_interface, 80, 88, RODAQ_STORAGE_PLUGIN_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_storage_open_request, target_uri, 24, 40, RODAQ_STORAGE_OPEN_REQUEST_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_storage_close_request, disposition, 4, 8, RODAQ_STORAGE_CLOSE_REQUEST_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_storage_submission, payload, 80, 96, RODAQ_STORAGE_SUBMISSION_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_storage_submit_result, persistence_error, 16, 344,
    RODAQ_STORAGE_SUBMIT_RESULT_MIN_STRUCT_SIZE);

RODAQ_EXPECT_MINIMUM_PREFIX(
    rodaq_runtime_storage, report_failure, 24, 32, RODAQ_RUNTIME_STORAGE_MIN_STRUCT_SIZE);

#undef RODAQ_EXPECT_MINIMUM_PREFIX

#endif /* RODAQ_TESTS_ABI_MINIMUM_PREFIX_CHECKS_H */
