#ifndef RODAQ_TEST_COMPAT_V1_FROZEN_OUTPUTS_H
#define RODAQ_TEST_COMPAT_V1_FROZEN_OUTPUTS_H

#include <stdint.h>

typedef struct {
    const char* data;
    uint64_t size;
} frozen_string_view_v1;
typedef struct {
    const uint8_t* data;
    uint64_t size;
} frozen_bytes_view_v1;
typedef struct {
    uint64_t value;
} frozen_descriptor_handle_v1;
typedef struct {
    uint64_t value;
} frozen_clock_handle_v1;
typedef struct {
    uint64_t value;
} frozen_storage_operation_v1;

typedef struct {
    uint32_t code;
    uint32_t flags;
    int64_t native_code;
    uint32_t domain_size;
    uint32_t message_size;
    char domain[32];
    char message[256];
    uint64_t reserved[2];
} frozen_error_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    frozen_descriptor_handle_v1 descriptor;
    frozen_string_view_v1 projection_id;
    frozen_bytes_view_v1 payload;
} frozen_projection_view_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    frozen_clock_handle_v1 clock;
    int64_t clock_reading;
    uint64_t unix_anchor_ns;
} frozen_clock_anchor_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t full_fidelity;
    uint32_t reserved;
    uint64_t maximum_payload_bytes;
    uint64_t maximum_inflight_records;
    uint64_t maximum_inflight_bytes;
    uint64_t handoff_progress_timeout_ns;
    uint64_t descriptor_progress_timeout_ns;
    uint64_t flush_progress_timeout_ns;
    uint64_t close_progress_timeout_ns;
} frozen_storage_capabilities_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t state;
    frozen_error_v1 failure;
} frozen_descriptor_readiness_result_v1;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t payload_state;
    uint32_t persistence_state;
    frozen_error_v1 persistence_error;
} frozen_storage_submit_result_v1;

#endif
