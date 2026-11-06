#ifndef RODAQ_PLUGIN_ABI_COMMON_H_
#define RODAQ_PLUGIN_ABI_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_PLUGIN_ABI_VERSION UINT32_C(1)

#if defined(__cplusplus)
#define RODAQ_NOEXCEPT noexcept
#else
#define RODAQ_NOEXCEPT
#endif

#if defined(_WIN32)
#define RODAQ_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define RODAQ_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define RODAQ_PLUGIN_EXPORT
#endif

typedef uint32_t rodaq_bool;

#define RODAQ_FALSE UINT32_C(0)
#define RODAQ_TRUE UINT32_C(1)

typedef uint32_t rodaq_status_code;

#define RODAQ_STATUS_OK UINT32_C(0)
#define RODAQ_STATUS_INVALID_ARGUMENT UINT32_C(1)
#define RODAQ_STATUS_INVALID_STATE UINT32_C(2)
#define RODAQ_STATUS_NOT_FOUND UINT32_C(3)
#define RODAQ_STATUS_CONFLICT UINT32_C(4)
#define RODAQ_STATUS_RESOURCE_EXHAUSTED UINT32_C(5)
#define RODAQ_STATUS_UNAVAILABLE UINT32_C(6)
#define RODAQ_STATUS_STORAGE_ERROR UINT32_C(7)
#define RODAQ_STATUS_TIMEOUT UINT32_C(8)
#define RODAQ_STATUS_INTERNAL_ERROR UINT32_C(9)
#define RODAQ_STATUS_UNKNOWN_ERROR UINT32_C(10)

/* ABI-FROZEN: borrowed UTF-8 bytes, not necessarily NUL-terminated. */
typedef struct rodaq_string_view {
    const char* data;
    uint64_t size;
} rodaq_string_view;

/* ABI-FROZEN: borrowed opaque bytes. */
typedef struct rodaq_bytes_view {
    const uint8_t* data;
    uint64_t size;
} rodaq_bytes_view;

#define RODAQ_STATUS_DOMAIN_CAPACITY UINT32_C(32)
#define RODAQ_STATUS_MESSAGE_CAPACITY UINT32_C(256)

#define RODAQ_STATUS_FLAG_MESSAGE_TRUNCATED UINT32_C(1)

/* ABI-FROZEN: caller-owned, self-contained diagnostic storage. */
typedef struct rodaq_status {
    rodaq_status_code status_code;
    uint32_t flags;

    int64_t error_code;

    uint32_t domain_size;
    uint32_t message_size;

    char domain[RODAQ_STATUS_DOMAIN_CAPACITY];
    char message[RODAQ_STATUS_MESSAGE_CAPACITY];

    uint64_t reserved[2];
} rodaq_status;

/* ABI-FROZEN: borrowed context/vtable pair. */
typedef struct rodaq_interface {
    void* context;
    const void* vtable;
} rodaq_interface;

#if defined(__cplusplus)
#define RODAQ_ABI_FIELD_SIZE(type, field) sizeof(decltype(static_cast<type*>(nullptr)->field))
#else
#define RODAQ_ABI_FIELD_SIZE(type, field) sizeof(((type*)0)->field)
#endif

#define RODAQ_HAS_FIELD(type, ptr, field) \
    ((ptr) != NULL &&                     \
     (ptr)->struct_size >= offsetof(type, field) + RODAQ_ABI_FIELD_SIZE(type, field))

#define RODAQ_HAS_FIELD_CAPACITY(type, capacity, field) \
    ((capacity) >= offsetof(type, field) + RODAQ_ABI_FIELD_SIZE(type, field))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_PLUGIN_ABI_COMMON_H_ */
