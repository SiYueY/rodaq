#ifndef RODAQ_C_ACQUISITION_H
#define RODAQ_C_ACQUISITION_H

#include <rodaq/c/canonical.h>
#include <rodaq/c/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_ACQUISITION_INTERFACE_ID "com.rodaq.acquisition"
#define RODAQ_ACQUISITION_INTERFACE_MAJOR 1u
#define RODAQ_ACQUISITION_INTERFACE_MINOR 0u

/**
 * FROZEN_VALUE array element; header is sanity only, not tail extension. Nonzero stable stream_id,
 * exact opaque UTF-8 stream_key and byte limit. Prepare requires a bijection with configured plugin
 * stream keys.
 */
typedef struct rodaq_acquisition_stream_binding {
    rodaq_struct_header_t header;
    rodaq_stream_id_t stream_id;
    rodaq_string_t stream_key;
    uint64_t max_record_bytes;
} rodaq_acquisition_stream_binding_t;

/**
 * EXTENSIBLE_STRUCT. streams/count and all nested views are borrowed until prepare actually
 * returns. Positive count requires aligned non-NULL array. max_callback_concurrency bounds
 * concurrent callbacks.
 */
typedef struct rodaq_acquisition_prepare_config {
    rodaq_struct_header_t header;
    const rodaq_acquisition_stream_binding_t* streams;
    uint32_t stream_count;
    uint32_t max_callback_concurrency;
} rodaq_acquisition_prepare_config_t;

typedef uint32_t rodaq_record_disposition_t;

enum {
    RODAQ_SUBMIT_INVALID = 0,
    RODAQ_SUBMIT_ACCEPTED = 1,
    RODAQ_SUBMIT_GATE_CLOSED = 2,
    RODAQ_SUBMIT_OBSOLETE_INSTANCE = 3,
    RODAQ_SUBMIT_STALE_DESCRIPTOR = 4,
    RODAQ_SUBMIT_DESCRIPTOR_NOT_READY = 5,
    RODAQ_SUBMIT_INVALID_RECORD = 6,
    RODAQ_SUBMIT_RECORD_TOO_LARGE = 7,
    RODAQ_SUBMIT_RESOURCE_EXHAUSTED = 8
};

typedef uint32_t rodaq_acquisition_event_kind_t;

enum {
    RODAQ_ACQUISITION_EVENT_INVALID = 0,
    RODAQ_ACQUISITION_EVENT_TRANSPORT_READY = 1,
    RODAQ_ACQUISITION_EVENT_TRANSPORT_LOST = 2,
    RODAQ_ACQUISITION_EVENT_STREAM_READY = 3,
    RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE = 4,
    RODAQ_ACQUISITION_EVENT_DROPPED = 5,
    RODAQ_ACQUISITION_EVENT_OVERFLOW = 6,
    RODAQ_ACQUISITION_EVENT_INVALID_PAYLOAD = 7,
    RODAQ_ACQUISITION_EVENT_SOURCE_TIMESTAMP_DISCONTINUITY = 8,
    RODAQ_ACQUISITION_EVENT_RECOVERABLE_FAILURE = 9,
    RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE = 10
};

/**
 * EXTENSIBLE_STRUCT. Bounded readiness/fault/evidence report, not a persistent event bus. Presence
 * flags are 0/1 and absent fields/reserved bytes are zero; coverage events require stream scope and
 * positive count.
 */
typedef struct rodaq_acquisition_event {
    rodaq_struct_header_t header;
    rodaq_acquisition_event_kind_t kind;
    rodaq_status_code_t status;
    rodaq_stream_id_t stream_id;
    rodaq_source_id_t source_id;
    uint64_t count;
    uint8_t has_stream_id;
    uint8_t has_source_id;
    uint8_t reserved[6];
} rodaq_acquisition_event_t;

/**
 * EXTENSIBLE_STRUCT; required prefix ends at metadata. Borrowed immutable bind-once descriptor
 * during prepare_descriptor; strings, schema and metadata views must be copied for retention.
 * Provider never supplies a schema digest; Core validates structure and normalizes metadata JSON
 * without interpreting backend-specific semantics.
 */
typedef struct rodaq_acquisition_descriptor {
    rodaq_struct_header_t header;
    rodaq_stream_id_t stream_id;
    rodaq_string_t channel_name;
    rodaq_string_t message_encoding;
    rodaq_string_t schema_name;
    rodaq_string_t schema_encoding;
    rodaq_bytes_t schema;
    rodaq_descriptor_metadata_t metadata;
} rodaq_acquisition_descriptor_t;

/**
 * EXTENSIBLE_STRUCT pointer-only input. Valid nonzero stream_id and bind-once descriptor_id.
 * Payload backing is COPY-only and remains borrowed until submit_record actually returns.
 * source_publish_time is nanoseconds (0 when !has_source_publish_time); reserved bytes are zero.
 */
typedef struct rodaq_acquisition_record_attempt {
    rodaq_struct_header_t header;
    rodaq_stream_id_t stream_id;
    rodaq_descriptor_id_t descriptor_id;
    rodaq_bytes_t payload;
    uint64_t source_publish_time;
    uint8_t has_source_publish_time;
    uint8_t reserved[7];
} rodaq_acquisition_record_attempt_t;

/**
 * EXTENSIBLE_STRUCT. Application-owned callback table passed to Acquisition prepare(). Valid for
 * the prepared Recording Instance until quiesce completes. prepare_descriptor runs during prepare
 * only (bind-once per stream); submit_record is bounded/non-blocking COPY ingress; report_event is
 * bounded readiness/fault reporting.
 */
typedef struct rodaq_acquisition_callbacks {
    rodaq_struct_header_t header;
    void* context;

    rodaq_status_code_t (*prepare_descriptor)(
        void* context, rodaq_stream_id_t stream_id,
        const rodaq_acquisition_descriptor_t* descriptor, rodaq_descriptor_id_t* descriptor_id,
        rodaq_error_t* error);

    rodaq_record_disposition_t (*submit_record)(
        void* context, const rodaq_acquisition_record_attempt_t* record);

    void (*report_event)(void* context, const rodaq_acquisition_event_t* event);
} rodaq_acquisition_callbacks_t;

/**
 * EXTENSIBLE_STRUCT; all three slots required in major 1. Non-NULL context/config/callbacks,
 * optional error. prepare borrows bindings, binds immutable stream descriptors via callbacks and
 * establishes sources; start enables bounded callbacks, not Core Admission. quiesce is idempotent
 * and OK proves callbacks settled and callbacks table will no longer be accessed. Mutating
 * lifecycle calls are serialized; no callback may synchronously reenter them. Operation deadlines
 * do not cancel native work.
 */
typedef struct rodaq_acquisition_interface_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*prepare)(
        rodaq_interface_context_t* context, const rodaq_acquisition_prepare_config_t* config,
        const rodaq_acquisition_callbacks_t* callbacks, rodaq_deadline_t deadline,
        rodaq_error_t* error);

    rodaq_status_code_t (*start)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*quiesce)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);
} rodaq_acquisition_interface_vtable_t;

#ifdef __cplusplus
}
#endif

#endif
