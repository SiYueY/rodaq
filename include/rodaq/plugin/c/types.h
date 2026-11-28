#ifndef RODAQ_PLUGIN_C_TYPES_H
#define RODAQ_PLUGIN_C_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t rodaq_status_code_t;

enum {
    RODAQ_STATUS_OK = 0u,
    RODAQ_STATUS_INVALID_ARGUMENT = 1u,
    RODAQ_STATUS_INVALID_STATE = 2u,
    RODAQ_STATUS_NOT_FOUND = 3u,
    RODAQ_STATUS_CONFLICT = 4u,
    RODAQ_STATUS_RESOURCE_EXHAUSTED = 5u,
    RODAQ_STATUS_DEADLINE_EXCEEDED = 6u,
    RODAQ_STATUS_UNAVAILABLE = 7u,
    RODAQ_STATUS_PERMISSION_DENIED = 8u,
    RODAQ_STATUS_DATA_LOSS = 9u,
    RODAQ_STATUS_INTERNAL = 10u
};

typedef uint32_t rodaq_log_level_t;

enum {
    RODAQ_LOG_TRACE = 0u,
    RODAQ_LOG_DEBUG = 1u,
    RODAQ_LOG_INFO = 2u,
    RODAQ_LOG_WARNING = 3u,
    RODAQ_LOG_ERROR = 4u,
    RODAQ_LOG_CRITICAL = 5u
};

/**
 * FROZEN_VALUE. Borrowed UTF-8 bytes, not NUL-terminated. NULL iff empty is allowed; NULL with
 * nonzero size is invalid. Synchronous inputs live until the native call returns.
 */
typedef struct rodaq_string {
    const char* data;
    uint32_t size;
} rodaq_string_t;

/**
 * FROZEN_VALUE. Borrowed bytes. NULL is permitted only when size is zero; the provider must copy
 * data needed beyond the native call.
 */
typedef struct rodaq_bytes {
    const uint8_t* data;
    uint32_t size;
} rodaq_bytes_t;

/**
 * FROZEN_VALUE. Caller-owned writable bytes. Provider may write content and size only. Caller
 * snapshots data/capacity before the call and verifies both before dereferencing output; size must
 * not exceed capacity.
 */
typedef struct rodaq_buffer {
    uint8_t* data;
    uint32_t capacity;
    uint32_t size;
} rodaq_buffer_t;

/**
 * FROZEN_VALUE. Absolute Linux CLOCK_MONOTONIC nanoseconds. INT64_MAX means infinite; other
 * negatives are invalid. Expiration never proves native work has returned or been cancelled.
 */
typedef struct rodaq_deadline {
    /**
     * Absolute CLOCK_MONOTONIC deadline.
     *
     * Unit: nanoseconds.
     */
    int64_t monotonic_deadline;
} rodaq_deadline_t;

#define RODAQ_DEADLINE_INFINITE INT64_MAX

/**
 * FROZEN_VALUE. struct_size is the actual readable/writable byte capacity. Extensible objects cross
 * the ABI by pointer only; readers use the common prefix, never sizeof equality.
 */
typedef struct rodaq_struct_header {
    uint32_t struct_size;
} rodaq_struct_header_t;

/**
 * FROZEN_VALUE. Optional diagnostic buffer; the status code is the only control result. UTF-8 code
 * points may not be split when truncating. No NUL is required. On OK, message.size must be zero.
 * Never change caller data/capacity, including on failure.
 */
typedef struct rodaq_error {
    rodaq_buffer_t message;
} rodaq_error_t;

#ifdef __cplusplus
}
#endif

#endif
