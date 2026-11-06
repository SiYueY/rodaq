#ifndef RODAQ_TEST_CURRENT_ABI_V1_BASELINE_OUTPUTS_H
#define RODAQ_TEST_CURRENT_ABI_V1_BASELINE_OUTPUTS_H

#include <stdint.h>

typedef struct {
    const char* data;
    uint64_t size;
} baseline_string_view;
typedef struct {
    const uint8_t* data;
    uint64_t size;
} baseline_bytes_view;
typedef struct {
    uint64_t value;
} baseline_descriptor_handle;
typedef struct {
    uint64_t value;
} baseline_clock_handle;
typedef struct {
    uint64_t value;
} baseline_storage_operation;

typedef struct {
    uint32_t status_code;
    uint32_t flags;
    int64_t error_code;
    uint32_t domain_size;
    uint32_t message_size;
    char domain[32];
    char message[256];
    uint64_t reserved[2];
} baseline_status;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    baseline_descriptor_handle descriptor;
    baseline_string_view projection_id;
    baseline_bytes_view payload;
} baseline_projection_view;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    baseline_clock_handle clock;
    int64_t clock_reading;
    uint64_t unix_anchor_ns;
} baseline_clock_anchor;

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
} baseline_storage_capabilities;

typedef struct {
    uint32_t struct_size;
    uint32_t state;
    baseline_status failure;
} baseline_descriptor_readiness_result;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t payload_state;
    uint32_t persistence_state;
    baseline_status persistence_error;
} baseline_storage_submit_result;

#endif
