#ifndef RODAQ_CANONICAL_H
#define RODAQ_CANONICAL_H

#include <rodaq/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI-FROZEN: zero is never a valid Runtime handle. */
typedef struct rodaq_source_handle {
    uint64_t value;
} rodaq_source_handle;

/* ABI-FROZEN: zero is never a valid Runtime handle. */
typedef struct rodaq_clock_handle {
    uint64_t value;
} rodaq_clock_handle;

/* ABI-FROZEN: zero is never a valid Runtime handle. */
typedef struct rodaq_schema_handle {
    uint64_t value;
} rodaq_schema_handle;

/* ABI-FROZEN: zero is never a valid Runtime handle. */
typedef struct rodaq_stream_handle {
    uint64_t value;
} rodaq_stream_handle;

/* ABI-FROZEN: zero is never a valid Runtime handle. */
typedef struct rodaq_stream_descriptor_handle {
    uint64_t value;
} rodaq_stream_descriptor_handle;

/* ABI-FROZEN: zero is never a valid operation handle. */
typedef struct rodaq_storage_operation {
    uint64_t value;
} rodaq_storage_operation;

/* ABI-FROZEN: Canonical timestamp key and Clock lookup handle. */
typedef struct rodaq_timestamp_key {
    rodaq_string_view role;
    rodaq_clock_handle clock;
} rodaq_timestamp_key;

/* ABI-FROZEN: explicit Absent or Present(reading) state. */
typedef struct rodaq_timestamp_value {
    rodaq_bool present;
    uint32_t reserved;

    int64_t reading;
} rodaq_timestamp_value;

/* ABI-APPENDABLE: source registration request. */
typedef struct rodaq_source_registration {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_bytes_view source_id;
} rodaq_source_registration;

/* ABI-APPENDABLE: Clock registration request. */
typedef struct rodaq_clock_registration {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_bytes_view native_id;

    uint64_t resolution_numerator;
    uint64_t resolution_denominator;

    rodaq_string_view origin;
} rodaq_clock_registration;

/* ABI-APPENDABLE: Schema registration request. */
typedef struct rodaq_schema_registration {
    uint32_t struct_size;

    rodaq_bool type_identifier_present;
    uint32_t reserved;

    rodaq_string_view format;
    rodaq_string_view type_identifier;
    rodaq_bytes_view representation;
} rodaq_schema_registration;

/* ABI-APPENDABLE: Stream registration request. */
typedef struct rodaq_stream_registration {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_bytes_view native_id;
} rodaq_stream_registration;

/* ABI-APPENDABLE: StreamDescriptor publication request. */
typedef struct rodaq_stream_descriptor_registration {
    uint32_t struct_size;

    rodaq_bool schema_present;
    uint32_t reserved;

    rodaq_stream_handle stream;

    rodaq_string_view name;

    rodaq_schema_handle schema;

    rodaq_string_view payload_encoding;

    const rodaq_timestamp_key* timestamp_keys;
    uint32_t timestamp_key_count;
    uint32_t reserved2;
} rodaq_stream_descriptor_registration;

/* ABI-APPENDABLE: caller-owned Source view. */
typedef struct rodaq_source_view {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_source_handle handle;

    rodaq_bytes_view source_id;
} rodaq_source_view;

/* ABI-APPENDABLE: caller-owned Clock view. */
typedef struct rodaq_clock_view {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_clock_handle handle;

    uint64_t clock_id;

    uint64_t resolution_numerator;
    uint64_t resolution_denominator;

    rodaq_string_view origin;
} rodaq_clock_view;

/* ABI-APPENDABLE: caller-owned Schema view. */
typedef struct rodaq_schema_view {
    uint32_t struct_size;

    rodaq_bool type_identifier_present;
    uint32_t reserved;

    rodaq_schema_handle handle;

    uint64_t schema_id;

    rodaq_string_view format;
    rodaq_string_view type_identifier;
    rodaq_bytes_view representation;
} rodaq_schema_view;

/* ABI-APPENDABLE: caller-owned TimestampKeySet view. */
typedef struct rodaq_timestamp_key_set_view {
    uint32_t struct_size;
    uint32_t reserved;

    uint64_t timestamp_key_set_id;

    const rodaq_timestamp_key* keys;
    uint32_t key_count;
    uint32_t reserved2;
} rodaq_timestamp_key_set_view;

/* ABI-APPENDABLE: caller-owned Stream view. */
typedef struct rodaq_stream_view {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_stream_handle handle;

    uint64_t stream_id;
} rodaq_stream_view;

/* ABI-APPENDABLE: caller-owned StreamDescriptor view. */
typedef struct rodaq_stream_descriptor_view {
    uint32_t struct_size;

    rodaq_bool schema_present;
    uint32_t reserved;

    rodaq_stream_descriptor_handle handle;
    rodaq_stream_handle stream;
    rodaq_schema_handle schema;

    uint64_t stream_descriptor_id;
    uint64_t stream_id;

    rodaq_string_view name;

    uint64_t schema_id;

    rodaq_string_view payload_encoding;

    uint64_t timestamp_key_set_id;

    const rodaq_timestamp_key* transport_timestamp_keys;
    uint32_t transport_timestamp_key_count;
    uint32_t reserved2;
} rodaq_stream_descriptor_view;

#define RODAQ_SOURCE_REGISTRATION_MIN_STRUCT_SIZE     \
    (offsetof(rodaq_source_registration, source_id) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_source_registration, source_id))

#define RODAQ_CLOCK_REGISTRATION_MIN_STRUCT_SIZE  \
    (offsetof(rodaq_clock_registration, origin) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_clock_registration, origin))

#define RODAQ_SCHEMA_REGISTRATION_MIN_STRUCT_SIZE          \
    (offsetof(rodaq_schema_registration, representation) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_schema_registration, representation))

#define RODAQ_STREAM_REGISTRATION_MIN_STRUCT_SIZE     \
    (offsetof(rodaq_stream_registration, native_id) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_stream_registration, native_id))

#define RODAQ_STREAM_DESCRIPTOR_REGISTRATION_MIN_STRUCT_SIZE               \
    (offsetof(rodaq_stream_descriptor_registration, timestamp_key_count) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_stream_descriptor_registration, timestamp_key_count))

#define RODAQ_SOURCE_VIEW_MIN_STRUCT_SIZE \
    (offsetof(rodaq_source_view, source_id) + RODAQ_ABI_FIELD_SIZE(rodaq_source_view, source_id))

#define RODAQ_CLOCK_VIEW_MIN_STRUCT_SIZE \
    (offsetof(rodaq_clock_view, origin) + RODAQ_ABI_FIELD_SIZE(rodaq_clock_view, origin))

#define RODAQ_SCHEMA_VIEW_MIN_STRUCT_SIZE          \
    (offsetof(rodaq_schema_view, representation) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_schema_view, representation))

#define RODAQ_TIMESTAMP_KEY_SET_VIEW_MIN_STRUCT_SIZE     \
    (offsetof(rodaq_timestamp_key_set_view, key_count) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_timestamp_key_set_view, key_count))

#define RODAQ_STREAM_VIEW_MIN_STRUCT_SIZE \
    (offsetof(rodaq_stream_view, stream_id) + RODAQ_ABI_FIELD_SIZE(rodaq_stream_view, stream_id))

#define RODAQ_STREAM_DESCRIPTOR_VIEW_MIN_STRUCT_SIZE                         \
    (offsetof(rodaq_stream_descriptor_view, transport_timestamp_key_count) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_stream_descriptor_view, transport_timestamp_key_count))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_CANONICAL_H */
