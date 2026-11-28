#ifndef RODAQ_C_CANONICAL_H
#define RODAQ_C_CANONICAL_H

#include <rodaq/c/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t rodaq_stream_id_t;
typedef uint64_t rodaq_descriptor_id_t;

#define RODAQ_SOURCE_ID_DATA_CAPACITY 24u

/**
 * FROZEN_VALUE, 32 bytes. Opaque per-stream source identity bytes (1..24 bytes when present).
 * Unused data[size..23] and reserved[0..3] bytes must be zero. Absent identity is all-zero.
 * Core never interprets middleware-specific source identity kinds.
 */
typedef struct rodaq_source_id {
    uint32_t size;
    uint8_t data[RODAQ_SOURCE_ID_DATA_CAPACITY];
    uint8_t reserved[4];
} rodaq_source_id_t;

/**
 * FROZEN_VALUE. Optional bounded format-tagged JSON metadata attached to a Stream Descriptor.
 * Either both format and json are empty ({NULL,0}) or both are present: format matches
 * ^[a-z0-9][a-z0-9._/-]{0,63}$ and json is a UTF-8 JSON object without duplicate keys or NUL.
 * Core validates structure and normalizes compact JSON without interpreting domain semantics.
 */
typedef struct rodaq_descriptor_metadata {
    rodaq_string_t format;
    rodaq_string_t json;
} rodaq_descriptor_metadata_t;

#ifdef __cplusplus
}
#endif

#endif
