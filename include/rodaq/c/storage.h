#ifndef RODAQ_C_STORAGE_H
#define RODAQ_C_STORAGE_H

#include <rodaq/c/canonical.h>
#include <rodaq/c/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_STORAGE_INTERFACE_ID "com.rodaq.storage"
#define RODAQ_STORAGE_INTERFACE_MAJOR 1u
#define RODAQ_STORAGE_INTERFACE_MINOR 0u

typedef uint64_t rodaq_segment_id_t;

typedef uint32_t rodaq_storage_tail_state_t;

enum {
    RODAQ_STORAGE_TAIL_NONE = 0,
    RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN = 1,
    RODAQ_STORAGE_TAIL_UNKNOWN = 2
};

/**
 * EXTENSIBLE_STRUCT. Core-owned workload envelope, not plugin JSON. *_bytes fields count bytes,
 * *_records count records, *_file_count counts files. Core owns coarse segment reservations and
 * recording_time computation; Storage never returns a per-record CapacityModel.
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
} rodaq_storage_prepare_config_t;

/**
 * FROZEN_VALUE batch element. Header is sanity only, no tail extension. Payload views live until
 * synchronous write_batch actually returns. recording_time is nanoseconds since Unix epoch computed
 * from the Recording Instance playback anchor. publish_time is optional source publication time in
 * nanoseconds (0 when !has_publish_time). reserved bytes are zero.
 */
typedef struct rodaq_storage_record {
    rodaq_struct_header_t header;

    rodaq_stream_id_t stream_id;
    rodaq_descriptor_id_t descriptor_id;

    uint64_t recording_time; /* Unit: nanoseconds since Unix epoch. */
    uint64_t publish_time;   /* Unit: nanoseconds; 0 when !has_publish_time. */

    rodaq_bytes_t payload;

    uint8_t has_publish_time;
    uint8_t reserved[7];
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
 * EXTENSIBLE_STRUCT; required prefix ends at metadata. Core-canonical immutable bind-once
 * descriptor with nonzero IDs. All schema/string/metadata views are borrowed until
 * prepare_descriptor returns; metadata JSON has been structure-validated and compact-normalized by
 * Core. No Acquisition config or caller-supplied digest.
 */
typedef struct rodaq_storage_descriptor {
    rodaq_struct_header_t header;
    rodaq_descriptor_id_t descriptor_id;
    rodaq_stream_id_t stream_id;
    rodaq_string_t channel_name;
    rodaq_string_t message_encoding;
    rodaq_string_t schema_name;
    rodaq_string_t schema_encoding;
    rodaq_bytes_t schema;
    rodaq_descriptor_metadata_t metadata;
} rodaq_storage_descriptor_t;

/**
 * EXTENSIBLE_STRUCT. segment_id is nonzero. Borrowed managed staging directory valid until
 * open_segment returns; copy/retain directory handle if needed. Plugin never chooses final path.
 */
typedef struct rodaq_segment_spec {
    rodaq_struct_header_t header;
    rodaq_segment_id_t segment_id;
    rodaq_string_t staging_directory;
} rodaq_segment_spec_t;

/**
 * EXTENSIBLE_STRUCT caller output, SuccessOnly. Reports current observed physical bytes and managed
 * file count of the active staging segment. Bounded in-memory read on Writer lane, no durable I/O.
 */
typedef struct rodaq_segment_usage {
    rodaq_struct_header_t header;

    uint64_t physical_bytes;
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

/**
 * EXTENSIBLE_STRUCT; all seven slots required in major 1. Non-NULL context/inputs/outputs; error
 * optional. One serialized mutable lane per instance, including late-return calls; never start a
 * second mutation after observer timeout. All deadlines are operation deadlines, segment_usage is
 * bounded/no I/O. write_batch requires nonempty aligned array; ValidOutcome after initial
 * validation, other outputs SuccessOnly. quiesce is idempotent and establishes no outstanding
 * writer/payload access; shutdown cannot invent successful seal or durable commit.
 */
typedef struct rodaq_storage_interface_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*prepare)(
        rodaq_interface_context_t* context, const rodaq_storage_prepare_config_t* config,
        rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*prepare_descriptor)(
        rodaq_interface_context_t* context, const rodaq_storage_descriptor_t* descriptor,
        rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*open_segment)(
        rodaq_interface_context_t* context, const rodaq_segment_spec_t* segment,
        rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*write_batch)(
        rodaq_interface_context_t* context, const rodaq_storage_record_t* records,
        uint32_t record_count, rodaq_deadline_t deadline,
        rodaq_storage_batch_result_t* batch_result, rodaq_error_t* error);

    rodaq_status_code_t (*segment_usage)(
        rodaq_interface_context_t* context, rodaq_segment_usage_t* usage, rodaq_error_t* error);

    rodaq_status_code_t (*seal_segment)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline,
        rodaq_segment_seal_result_t* seal_result, rodaq_error_t* error);

    rodaq_status_code_t (*quiesce)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);
} rodaq_storage_interface_vtable_t;

#ifdef __cplusplus
}
#endif

#endif
