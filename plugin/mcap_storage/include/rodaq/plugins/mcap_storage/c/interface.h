#pragma once

#include <rodaq/plugin/c/abi.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct rodaq_reference_mcap_context rodaq_reference_mcap_context_t;

typedef struct rodaq_reference_mcap_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*open)(
        rodaq_reference_mcap_context_t* context, rodaq_string_t path, rodaq_error_t* error);

    rodaq_status_code_t (*register_channel)(
        rodaq_reference_mcap_context_t* context, rodaq_string_t topic, rodaq_string_t message_type,
        rodaq_string_t schema_encoding, rodaq_bytes_t schema, rodaq_string_t message_encoding,
        uint64_t* channel_id, rodaq_error_t* error);

    rodaq_status_code_t (*write)(
        rodaq_reference_mcap_context_t* context, uint64_t channel_id, int64_t timestamp,
        rodaq_bytes_t payload, rodaq_error_t* error);

    rodaq_status_code_t (*close)(rodaq_reference_mcap_context_t* context, rodaq_error_t* error);
} rodaq_reference_mcap_vtable_t;

#if defined(__cplusplus)
}
#endif
