#ifndef RODAQ_STORAGE_C_TYPES_H
#define RODAQ_STORAGE_C_TYPES_H

#include <rodaq/canonical/c/types.h>
#include <rodaq/canonical/c/ros2.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_STORAGE_TIMESTAMP_CAPACITY 6u

typedef uint64_t rodaq_segment_id_t;

typedef uint32_t rodaq_storage_tail_state_t;

enum {
    RODAQ_STORAGE_TAIL_NONE = 0,
    RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN = 1,
    RODAQ_STORAGE_TAIL_UNKNOWN = 2
};

/**
 * EXTENSIBLE_STRUCT. Core-owned workload/time envelope, not plugin JSON. *_bytes fields count
 * bytes, *_records count records, *_file_count counts files. system_boot_id is nonzero; both
 * anchors are sampled together and frozen for the boot. playback_anchor_monotonic is
 * CLOCK_MONOTONIC nanoseconds; realtime is Unix epoch nanoseconds.
 */
typedef struct rodaq_storage_prepare_config {
    rodaq_struct_header_t header;

    uint64_t max_record_bytes;
    uint64_t max_batch_bytes;
    uint32_t max_batch_records;
    uint32_t max_descriptors_per_segment;

    uint64_t max_segment_records;
    uint64_t max_segment_input_bytes;
    uint64_t max_segment_physical_bytes;

    uint32_t max_segment_file_count;
    uint32_t max_relative_path_bytes;

    rodaq_system_boot_id_t system_boot_id;
    uint64_t playback_anchor_monotonic; /* Unit: nanoseconds. */
    int64_t playback_anchor_realtime;   /* Unit: nanoseconds since Unix epoch. */
} rodaq_storage_prepare_config_t;

/**
 * EXTENSIBLE_STRUCT caller output, SuccessOnly. Caller zeroes business fields and advertises actual
 * capacity in header. Provider writes common prefix only. Denominators are nonzero. Expansion is
 * checked ceil(bytes*numerator/denominator)+overhead; fixed overhead and seal reserve are byte
 * bounds, never averages. Frozen after successful prepare.
 */
typedef struct rodaq_storage_capacity_model {
    rodaq_struct_header_t header;

    uint64_t payload_expansion_numerator;
    uint64_t payload_expansion_denominator;
    uint64_t per_record_overhead_bytes;

    uint64_t descriptor_expansion_numerator;
    uint64_t descriptor_expansion_denominator;
    uint64_t per_descriptor_overhead_bytes;

    uint64_t fixed_segment_overhead_bytes;
    uint64_t seal_reserve_bytes;
} rodaq_storage_capacity_model_t;

/**
 * FROZEN_VALUE batch element. Header is sanity only, no tail extension. Payload/nested views live
 * until synchronous write actually returns. Nonzero stream/descriptor/sequence/boot/epoch facts.
 * timestamp_count <= 6; unused/absent/reserved fields are zero. Only RETAINED delivery bit is
 * supported. Boot must match prepared playback anchors.
 */
typedef struct rodaq_storage_record {
    rodaq_struct_header_t header;

    rodaq_stream_id_t stream_id;
    rodaq_descriptor_id_t descriptor_id;
    uint64_t acceptance_sequence;

    rodaq_system_boot_id_t system_boot_id;
    rodaq_clock_epoch_id_t clock_epoch_id;
    /** Core receive time in the recorded boot's CLOCK_MONOTONIC domain. Unit: nanoseconds. */
    uint64_t receive_monotonic;
    /** Core receive wall-clock time. Unit: nanoseconds since Unix epoch. */
    int64_t receive_realtime;

    rodaq_source_id_t source_id;
    rodaq_source_session_id_t source_session_id;
    uint64_t source_sequence;

    rodaq_bytes_t payload;
    rodaq_record_timestamp_t timestamps[RODAQ_STORAGE_TIMESTAMP_CAPACITY];

    uint32_t timestamp_count;
    rodaq_sequence_scope_t sequence_scope;
    rodaq_record_delivery_flags_t delivery_flags;
    uint8_t has_source_id;
    uint8_t has_source_session_id;
    uint8_t has_source_sequence;
    uint8_t reserved[5];
} rodaq_storage_record_t;

/**
 * EXTENSIBLE_STRUCT caller output, ValidOutcome after initial ABI validation. confirmed_prefix <=
 * record_count. TAIL_NONE iff full prefix. OK requires full prefix; non-OK may still confirm all
 * records. Before backend write, non-OK is 0/DEFINITELY_UNWRITTEN; uncertain suffix is UNKNOWN. Do
 * not discard this result merely because status is non-OK.
 */
typedef struct rodaq_storage_batch_result {
    rodaq_struct_header_t header;
    uint32_t confirmed_prefix;
    rodaq_storage_tail_state_t tail_state;
} rodaq_storage_batch_result_t;

/**
 * EXTENSIBLE_STRUCT pointer input. Core-canonical immutable descriptor, with nonzero persistent
 * IDs. All schema/string/QoS views are borrowed until prepare_descriptor returns; no Acquisition
 * config or caller-supplied digest.
 */
typedef struct rodaq_storage_descriptor {
    rodaq_struct_header_t header;
    rodaq_descriptor_id_t descriptor_id;
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
} rodaq_storage_descriptor_t;

/**
 * EXTENSIBLE_STRUCT. segment_id is nonzero and never reused across the managed deployment. Borrowed
 * managed staging directory valid until open returns; copy/retain directory handle if needed.
 * Plugin never chooses the final publish path.
 */
typedef struct rodaq_segment_spec {
    rodaq_struct_header_t header;
    rodaq_segment_id_t segment_id;
    rodaq_string_t staging_directory;
} rodaq_segment_spec_t;

/**
 * EXTENSIBLE_STRUCT caller output, SuccessOnly. Byte counters distinguish physical files from
 * pending conservative physical expansion. file_count counts existing managed files, reserved0
 * zero. Bounded in-memory read on Writer lane, no durable I/O.
 */
typedef struct rodaq_segment_usage {
    rodaq_struct_header_t header;

    uint64_t physical_bytes;
    uint64_t pending_physical_upper_bound;

    uint32_t file_count;
    uint32_t reserved0;
} rodaq_segment_usage_t;

/**
 * FROZEN_VALUE. Caller owns data/capacity including NUL space. Provider writes bytes and size
 * excluding NUL; size < capacity and data[size] is NUL. UTF-8 direct regular-file name only: no
 * directory separators, traversal or links. Never replace data/capacity.
 */
typedef struct rodaq_path_buffer {
    char* data;
    uint32_t capacity;
    uint32_t size;
} rodaq_path_buffer_t;

/**
 * FROZEN_VALUE array element. physical_size counts bytes. Nested path backing remains caller-owned
 * on every status.
 */
typedef struct rodaq_sealed_file {
    rodaq_path_buffer_t relative_path;
    uint64_t physical_size;
} rodaq_sealed_file_t;

/**
 * EXTENSIBLE_STRUCT caller output, SuccessOnly. Preallocate files and each path from prepared
 * limits; header advertises actual writable capacity. Snapshot files/file_capacity and ALL nested
 * path data/capacity before native call. On return validate header and all identities before
 * following any pointer. Unused business fields/backing remain zero; input pointers/capacities
 * remain intact. total_physical_bytes is checked sum. Seal is single-pass, never retry for capacity
 * discovery. OK relinquishes all plugin file mutation; Core alone owns durability/publish.
 */
typedef struct rodaq_segment_seal_result {
    rodaq_struct_header_t header;
    rodaq_sealed_file_t* files;
    uint32_t file_capacity;
    uint32_t file_count;
    uint64_t total_physical_bytes;
} rodaq_segment_seal_result_t;
#ifdef __cplusplus
}
#endif

#endif
