#ifndef RODAQ_CANONICAL_C_TYPES_H
#define RODAQ_CANONICAL_C_TYPES_H

#include <rodaq/plugin/c/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t rodaq_stream_id_t;
typedef uint64_t rodaq_descriptor_id_t;

typedef uint32_t rodaq_data_profile_t;
enum {
    RODAQ_DATA_PROFILE_INVALID = 0,
    RODAQ_DATA_PROFILE_GENERIC = 1,
    RODAQ_DATA_PROFILE_ROS2 = 2
};

#define RODAQ_SOURCE_ID_DATA_CAPACITY 24u

typedef uint32_t rodaq_source_id_kind_t;
enum { RODAQ_SOURCE_ID_INVALID = 0, RODAQ_SOURCE_ID_ROS2_GID = 1, RODAQ_SOURCE_ID_RTPS_GUID = 2 };

/**
 * FROZEN_VALUE, 32 bytes. ROS2_GID stores native bytes (up to 24), RTPS_GUID stores all 16
 * wire-order bytes. size is 1..24 for present IDs. Unused data and reserved bytes are zero. INVALID
 * is all-zero; never hash or truncate native identity.
 */
typedef struct rodaq_source_id {
    rodaq_source_id_kind_t kind;
    uint8_t size;
    uint8_t data[RODAQ_SOURCE_ID_DATA_CAPACITY];
    uint8_t reserved[3];
} rodaq_source_id_t;

#ifdef __cplusplus
}
#endif

#endif
