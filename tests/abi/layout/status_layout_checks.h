#ifndef RODAQ_TESTS_ABI_STATUS_LAYOUT_CHECKS_H
#define RODAQ_TESTS_ABI_STATUS_LAYOUT_CHECKS_H

#include <rodaq/plugin/abi/runtime.h>

#if defined(__cplusplus)
#define RODAQ_TEST_STATIC_ASSERT static_assert
#define RODAQ_TEST_ALIGNOF(type) alignof(type)
#else
#define RODAQ_TEST_STATIC_ASSERT _Static_assert
#define RODAQ_TEST_ALIGNOF(type) _Alignof(type)
#endif

RODAQ_TEST_STATIC_ASSERT(sizeof(void*) == 8, "V1 targets LP64 platforms");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_string_view) == 16, "string view size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_string_view) == 8, "string view align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_string_view, data) == 0, "string data offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_string_view, size) == 8, "string size offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_bytes_view) == 16, "bytes view size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_bytes_view) == 8, "bytes view align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_bytes_view, data) == 0, "bytes data offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_bytes_view, size) == 8, "bytes size offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_status) == 328, "error size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_status) == 8, "error align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, status_code) == 0, "error code offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, flags) == 4, "error flags offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, error_code) == 8, "status error code offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, domain_size) == 16, "error domain size offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, message_size) == 20, "error message size offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, domain) == 24, "error domain offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, message) == 56, "error message offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_status, reserved) == 312, "error reserved offset");

RODAQ_TEST_STATIC_ASSERT(
    sizeof(rodaq_descriptor_readiness_result) == 336, "descriptor readiness size");
RODAQ_TEST_STATIC_ASSERT(
    RODAQ_TEST_ALIGNOF(rodaq_descriptor_readiness_result) == 8, "descriptor readiness align");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_descriptor_readiness_result, struct_size) == 0,
    "descriptor readiness struct size offset");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_descriptor_readiness_result, state) == 4, "descriptor readiness state offset");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_descriptor_readiness_result, failure) == 8,
    "descriptor readiness failure offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_interface) == 16, "interface size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_interface) == 8, "interface align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_interface, context) == 0, "interface context offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_interface, vtable) == 8, "interface vtable offset");

#define RODAQ_ASSERT_HANDLE_LAYOUT(type)                                     \
    RODAQ_TEST_STATIC_ASSERT(sizeof(type) == 8, #type " size");              \
    RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(type) == 8, #type " align"); \
    RODAQ_TEST_STATIC_ASSERT(offsetof(type, value) == 0, #type " value offset")

RODAQ_ASSERT_HANDLE_LAYOUT(rodaq_source_handle);
RODAQ_ASSERT_HANDLE_LAYOUT(rodaq_clock_handle);
RODAQ_ASSERT_HANDLE_LAYOUT(rodaq_schema_handle);
RODAQ_ASSERT_HANDLE_LAYOUT(rodaq_stream_handle);
RODAQ_ASSERT_HANDLE_LAYOUT(rodaq_stream_descriptor_handle);
RODAQ_ASSERT_HANDLE_LAYOUT(rodaq_storage_operation);
RODAQ_ASSERT_HANDLE_LAYOUT(rodaq_binding_transition);

#undef RODAQ_ASSERT_HANDLE_LAYOUT

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_timestamp_key) == 24, "timestamp key size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_timestamp_key) == 8, "timestamp key align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_timestamp_key, role) == 0, "timestamp role offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_timestamp_key, clock) == 16, "timestamp clock offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_timestamp_value) == 16, "timestamp value size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_timestamp_value) == 8, "timestamp value align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_timestamp_value, present) == 0, "timestamp present offset");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_timestamp_value, reserved) == 4, "timestamp reserved offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_timestamp_value, reading) == 8, "timestamp reading offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_payload_input) == 40, "payload input size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_payload_input) == 8, "payload input align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_payload_input, mode) == 0, "payload mode offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_payload_input, reserved) == 4, "payload reserved offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_payload_input, data) == 8, "payload data offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_payload_input, size) == 16, "payload size offset");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_payload_input, owner_context) == 24, "payload owner offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_payload_input, release) == 32, "payload release offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_runtime_lifecycle_snapshot) == 16, "lifecycle snapshot size");
RODAQ_TEST_STATIC_ASSERT(
    RODAQ_TEST_ALIGNOF(rodaq_runtime_lifecycle_snapshot) == 8, "lifecycle snapshot align");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_lifecycle_snapshot, state) == 0, "lifecycle state offset");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_lifecycle_snapshot, normal_acquisition_allowed) == 4,
    "lifecycle permission offset");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_runtime_lifecycle_snapshot, generation) == 8, "lifecycle generation offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_service_registry) == 16, "service registry size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_service_registry) == 8, "service registry align");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_service_registry, context) == 0, "service registry context offset");
RODAQ_TEST_STATIC_ASSERT(
    offsetof(rodaq_service_registry, vtable) == 8, "service registry vtable offset");

RODAQ_TEST_STATIC_ASSERT(sizeof(rodaq_config_view) == 16, "config view size");
RODAQ_TEST_STATIC_ASSERT(RODAQ_TEST_ALIGNOF(rodaq_config_view) == 8, "config view align");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_config_view, context) == 0, "config context offset");
RODAQ_TEST_STATIC_ASSERT(offsetof(rodaq_config_view, vtable) == 8, "config vtable offset");

#endif /* RODAQ_TESTS_ABI_STATUS_LAYOUT_CHECKS_H */
