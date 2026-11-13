#pragma once

#include <rodaq/plugin/c/abi.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct rodaq_reference_ros2_control_context rodaq_reference_ros2_control_context_t;

typedef struct rodaq_reference_ros2_control_vtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*start)(
        rodaq_reference_ros2_control_context_t* context, rodaq_error_t* error);
    rodaq_status_code_t (*stop)(
        rodaq_reference_ros2_control_context_t* context, rodaq_error_t* error);
} rodaq_reference_ros2_control_vtable_t;

typedef struct rodaq_reference_ros2_sample {
    rodaq_struct_header_t header;
    rodaq_string_t topic;
    rodaq_string_t message_type;
    rodaq_string_t message_encoding;
    rodaq_string_t schema_encoding;
    rodaq_bytes_t schema;
    int64_t timestamp;
    rodaq_bytes_t payload;
} rodaq_reference_ros2_sample_t;

typedef struct rodaq_reference_ros2_sink_context rodaq_reference_ros2_sink_context_t;

typedef struct rodaq_reference_ros2_sink_vtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*record)(
        rodaq_reference_ros2_sink_context_t* context, const rodaq_reference_ros2_sample_t* sample,
        rodaq_error_t* error);
} rodaq_reference_ros2_sink_vtable_t;

#if defined(__cplusplus)
}
#endif
