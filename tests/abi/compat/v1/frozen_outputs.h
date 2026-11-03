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

#endif
