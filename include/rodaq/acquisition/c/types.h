#ifndef RODAQ_ACQUISITION_C_TYPES_H
#define RODAQ_ACQUISITION_C_TYPES_H

#include <rodaq/canonical/c/types.h>
#include <rodaq/canonical/c/ros2.h>

#ifdef __cplusplus
extern "C" {
#endif

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

typedef uint32_t rodaq_submit_disposition_t;

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

/**
 * EXTENSIBLE_STRUCT caller output. Initialize header capacity and disposition INVALID. SuccessOnly:
 * non-OK has no readable disposition. OK may express business rejection; only ACCEPTED means
 * admission committed. reserved0 is zero.
 */
typedef struct rodaq_submit_result {
    rodaq_struct_header_t header;
    rodaq_submit_disposition_t disposition;
    uint32_t reserved0;
} rodaq_submit_result_t;

#define RODAQ_RECORD_TIMESTAMP_CAPACITY 4u

typedef uint32_t rodaq_source_event_kind_t;

enum {
    RODAQ_SOURCE_EVENT_INVALID = 0,
    RODAQ_SOURCE_EVENT_DISCOVERED = 1,
    RODAQ_SOURCE_EVENT_LOST = 2,
    RODAQ_SOURCE_EVENT_SESSION_STARTED = 3,
    RODAQ_SOURCE_EVENT_SESSION_ENDED = 4,
    RODAQ_SOURCE_EVENT_SEQUENCE_GAP = 5,
    RODAQ_SOURCE_EVENT_SEQUENCE_RESET = 6
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
 * EXTENSIBLE_STRUCT, pointer-only subscription observation. Strings are borrowed for the actual
 * proposal call, nonempty UTF-8 without NUL (distribution <=64, RMW <=128 bytes). The request is
 * from an actually installed subscription, never an adapt candidate. Nonzero generation does not
 * wrap. executor_kind=1 is rclcpp MultiThreadedExecutor; actual thread count is 1..8.
 */
typedef struct rodaq_ros2_subscription_facts {
    rodaq_struct_header_t header;
    rodaq_string_t ros_distribution;
    rodaq_string_t rmw_implementation;
    rodaq_ros2_qos_profile_t installed_requested_qos;
    uint64_t subscription_generation;
    uint32_t executor_kind;
    uint32_t executor_threads;
} rodaq_ros2_subscription_facts_t;

/**
 * EXTENSIBLE_STRUCT. Borrowed immutable proposal during the call; strings/schema/QoS arrays must be
 * copied for retention. Generic profile has no ROS2 hash/QoS. ROS2 requires cdr plus supported
 * ros2msg/ros2idl schema. Provider never supplies a schema digest.
 */
typedef struct rodaq_descriptor_proposal {
    rodaq_struct_header_t header;
    rodaq_stream_id_t stream_id;
    rodaq_data_profile_t data_profile;
    uint32_t reserved0;
    rodaq_string_t channel_name;
    rodaq_string_t message_encoding;
    rodaq_string_t schema_name;
    rodaq_string_t schema_encoding;
    rodaq_bytes_t schema;
    rodaq_string_t ros2_type_description_hash;
    const rodaq_ros2_qos_profile_t* ros2_offered_qos_profiles;
    uint32_t ros2_offered_qos_profile_count;
    uint32_t reserved1;
    // Service minor 1 tail. Absent for an older complete prefix or NULL pointer.
    const rodaq_ros2_subscription_facts_t* ros2_subscription;
} rodaq_descriptor_proposal_t;

/**
 * EXTENSIBLE_STRUCT. Producer identity/session/sequence facts. Presence flags are 0/1; absent
 * fields and reserved bytes are zero. Report failures mark evidence incomplete rather than relying
 * on logs.
 */
typedef struct rodaq_source_event {
    rodaq_struct_header_t header;
    rodaq_stream_id_t stream_id;
    rodaq_source_id_t source_id;
    rodaq_source_session_id_t source_session_id;
    uint64_t source_sequence;
    rodaq_source_event_kind_t kind;
    rodaq_sequence_scope_t sequence_scope;
    uint8_t has_source_id;
    uint8_t has_source_session_id;
    uint8_t has_source_sequence;
    uint8_t reserved[5];
} rodaq_source_event_t;

/**
 * FROZEN_VALUE borrowed COPY-only payload. logical_size is bytes; positive size requires non-NULL
 * data. Valid until submit_record actually returns, including after observer timeout. No ownership
 * transfer, acquire/release or plugin-owned payload survives return.
 */
typedef struct rodaq_payload {
    rodaq_struct_header_t header;
    const uint8_t* data;
    uint64_t logical_size;
} rodaq_payload_t;

/**
 * EXTENSIBLE_STRUCT pointer-only input. Valid nonzero stream/token; timestamp_count <= 4. Unused
 * timestamps, absent source facts and reserved fields are zero. Unknown enum/flag values fail
 * closed. Payload backing remains borrowed until native return.
 */
typedef struct rodaq_record_attempt {
    rodaq_struct_header_t header;

    rodaq_stream_id_t stream_id;
    rodaq_descriptor_token_t descriptor_token;
    rodaq_source_id_t source_id;
    rodaq_source_session_id_t source_session_id;
    uint64_t source_sequence;

    rodaq_payload_t payload;
    rodaq_record_timestamp_t timestamps[RODAQ_RECORD_TIMESTAMP_CAPACITY];

    rodaq_sequence_scope_t sequence_scope;
    uint32_t timestamp_count;
    rodaq_record_delivery_flags_t delivery_flags;
    uint8_t has_source_id;
    uint8_t has_source_session_id;
    uint8_t has_source_sequence;
    uint8_t reserved[5];
} rodaq_record_attempt_t;

#ifdef __cplusplus
}
#endif

#endif
