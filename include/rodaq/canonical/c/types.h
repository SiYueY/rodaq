#ifndef RODAQ_CANONICAL_C_TYPES_H
#define RODAQ_CANONICAL_C_TYPES_H

#include <rodaq/plugin/c/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t rodaq_stream_id_t;
typedef uint64_t rodaq_descriptor_id_t;
typedef uint64_t rodaq_system_boot_id_t;
typedef uint64_t rodaq_clock_epoch_id_t;
typedef uint64_t rodaq_source_session_id_t;

typedef uint32_t rodaq_data_profile_t;
enum {
    RODAQ_DATA_PROFILE_INVALID = 0,
    RODAQ_DATA_PROFILE_GENERIC = 1,
    RODAQ_DATA_PROFILE_ROS2 = 2
};

typedef uint32_t rodaq_sequence_scope_t;
enum {
    RODAQ_SEQUENCE_SCOPE_INVALID = 0,
    RODAQ_SEQUENCE_SCOPE_OPAQUE = 1,
    RODAQ_SEQUENCE_SCOPE_SOURCE = 2,
    RODAQ_SEQUENCE_SCOPE_SOURCE_SESSION = 3
};

typedef uint32_t rodaq_record_delivery_flags_t;
enum { RODAQ_RECORD_DELIVERY_NONE = 0u, RODAQ_RECORD_DELIVERY_RETAINED = 1u << 0 };

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

/**
 * FROZEN_VALUE, 16 bytes. Runtime-only binding token; value and generation are nonzero. Not a
 * persistent descriptor_id.
 */
typedef struct rodaq_descriptor_token {
    uint64_t value;
    uint64_t generation;
} rodaq_descriptor_token_t;

typedef uint32_t rodaq_timestamp_role_t;
enum { RODAQ_TIMESTAMP_ROLE_INVALID = 0, RODAQ_TIMESTAMP_ROLE_SOURCE_PUBLICATION = 1 };

typedef uint32_t rodaq_clock_domain_t;
enum {
    RODAQ_CLOCK_DOMAIN_INVALID = 0,
    RODAQ_CLOCK_DOMAIN_UNIX_EPOCH = 1,
    RODAQ_CLOCK_DOMAIN_SOURCE_NATIVE = 2
};

typedef uint32_t rodaq_timestamp_flags_t;
enum { RODAQ_TIMESTAMP_FLAGS_NONE = 0u };

/**
 * FROZEN_VALUE, 32 bytes. value is nanoseconds. V1 permits SOURCE_PUBLICATION only; UNIX_EPOCH
 * requires clock_id zero, SOURCE_NATIVE requires nonzero. flags/reserved are zero. Presence is
 * represented by array count.
 */
typedef struct rodaq_record_timestamp {
    rodaq_timestamp_role_t role;
    rodaq_clock_domain_t clock_domain;
    uint64_t clock_id;
    int64_t value;
    rodaq_timestamp_flags_t flags;
    uint32_t reserved;
} rodaq_record_timestamp_t;

#ifdef __cplusplus
}
#endif

#endif
