#pragma once

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(_WIN32)
#define RODAQ_PLUGIN_EXPORT_SYMBOL __declspec(dllexport)
#else
#define RODAQ_PLUGIN_EXPORT_SYMBOL __attribute__((visibility("default")))
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

typedef struct rodaq_string {
    const char* data;
    uint32_t size;
} rodaq_string_t;

typedef struct rodaq_bytes {
    const uint8_t* data;
    uint32_t size;
} rodaq_bytes_t;

typedef struct rodaq_buffer {
    uint8_t* data;
    uint32_t capacity;
    uint32_t size;
} rodaq_buffer_t;

typedef struct rodaq_deadline {
    int64_t monotonic_deadline;
} rodaq_deadline_t;

#define RODAQ_DEADLINE_INFINITE INT64_MAX

typedef struct rodaq_struct_header {
    uint32_t struct_size;
} rodaq_struct_header_t;

typedef struct rodaq_error {
    rodaq_buffer_t message;
} rodaq_error_t;

typedef struct rodaq_interface_version {
    uint16_t major;
    uint16_t minor;
} rodaq_interface_version_t;

typedef struct rodaq_interface_query {
    rodaq_string_t interface_id;
    uint16_t major;
    uint16_t min_minor;
    uint16_t max_minor;
} rodaq_interface_query_t;

typedef struct rodaq_interface_context rodaq_interface_context_t;
typedef struct rodaq_interface_vtable rodaq_interface_vtable_t;

typedef struct rodaq_interface_reference {
    rodaq_interface_context_t* context;
    const rodaq_interface_vtable_t* vtable;
    uint32_t vtable_size;
    rodaq_interface_version_t negotiated_version;
} rodaq_interface_reference_t;

typedef struct rodaq_service_registry_context rodaq_service_registry_context_t;

typedef struct rodaq_service_registry_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*query)(
        rodaq_service_registry_context_t* context, const rodaq_interface_query_t* query,
        rodaq_interface_reference_t* interface, rodaq_error_t* error);

    rodaq_status_code_t (*enter_call)(
        rodaq_service_registry_context_t* context, rodaq_error_t* error);

    void (*leave_call)(rodaq_service_registry_context_t* context);
} rodaq_service_registry_vtable_t;

typedef struct rodaq_service_registry_reference {
    rodaq_service_registry_context_t* context;
    const rodaq_service_registry_vtable_t* vtable;
} rodaq_service_registry_reference_t;

typedef struct rodaq_plugin_context rodaq_plugin_context_t;
typedef struct rodaq_plugin_instance_vtable rodaq_plugin_instance_vtable_t;

typedef struct rodaq_plugin_instance {
    rodaq_plugin_context_t* context;
    const rodaq_plugin_instance_vtable_t* vtable;
} rodaq_plugin_instance_t;

typedef struct rodaq_plugin_descriptor {
    rodaq_struct_header_t header;
    rodaq_string_t plugin_id;
    rodaq_string_t plugin_name;
    rodaq_string_t plugin_version;
} rodaq_plugin_descriptor_t;

typedef struct rodaq_config {
    rodaq_struct_header_t header;
    rodaq_string_t json;
} rodaq_config_t;

struct rodaq_plugin_instance_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*configure)(
        rodaq_plugin_context_t* context, const rodaq_service_registry_reference_t* services,
        const rodaq_config_t* config, rodaq_error_t* error);

    rodaq_status_code_t (*query)(
        rodaq_plugin_context_t* context, const rodaq_interface_query_t* query,
        rodaq_interface_reference_t* interface, rodaq_error_t* error);

    rodaq_status_code_t (*shutdown)(
        rodaq_plugin_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);

    void (*destroy)(rodaq_plugin_context_t* context);
};

typedef struct rodaq_plugin_entry {
    rodaq_struct_header_t header;

    const rodaq_plugin_descriptor_t* (*descriptor)(void);

    rodaq_status_code_t (*create)(rodaq_plugin_instance_t* instance, rodaq_error_t* error);
} rodaq_plugin_entry_t;

#define RODAQ_PLUGIN_ABI_VERSION(major, minor) \
    ((((uint32_t)(major)) << 16u) | ((uint32_t)(minor)&0xffffu))
#define RODAQ_PLUGIN_ABI_MAJOR(value) ((uint16_t)(((uint32_t)(value)) >> 16u))
#define RODAQ_PLUGIN_ABI_MINOR(value) ((uint16_t)(((uint32_t)(value)) & 0xffffu))

typedef uint32_t (*rodaq_plugin_abi_version_fn_t)(void);
typedef const rodaq_plugin_entry_t* (*rodaq_plugin_get_entry_fn_t)(void);

RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void);
RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void);

#if defined(__cplusplus)
}
#endif
