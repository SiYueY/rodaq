#ifndef RODAQ_PLUGIN_C_PLUGIN_H
#define RODAQ_PLUGIN_C_PLUGIN_H

#include <rodaq/plugin/c/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rodaq_interface_context rodaq_interface_context_t;

typedef struct rodaq_interface_vtable rodaq_interface_vtable_t;

typedef struct rodaq_plugin_context rodaq_plugin_context_t;

/**
 * FROZEN_VALUE. Negotiated Interface version; separate from Plugin ABI and Plugin release version.
 */
typedef struct rodaq_interface_version {
    uint16_t major;
    uint16_t minor;
} rodaq_interface_version_t;

/**
 * FROZEN_VALUE. interface_id is 1..255 lowercase ASCII [a-z0-9._-]. Major matches exactly;
 * min_minor <= max_minor. Select the highest shared minor. Missing ID is NOT_FOUND; no version
 * intersection is UNAVAILABLE.
 */
typedef struct rodaq_interface_query {
    rodaq_string_t interface_id;
    uint16_t major;
    uint16_t min_minor;
    uint16_t max_minor;
} rodaq_interface_query_t;

/**
 * FROZEN_VALUE. Borrowed instance interface reference. Default-invalid is all zero. Validate
 * context, vtable and vtable_size before reading its header; readable bytes are min(vtable_size,
 * header.struct_size, consumer known size). Check required prefix and each negotiated slot before
 * access.
 */
typedef struct rodaq_interface_reference {
    rodaq_interface_context_t* context;
    const rodaq_interface_vtable_t* vtable;
    uint32_t vtable_size;
    rodaq_interface_version_t negotiated_version;
} rodaq_interface_reference_t;

/**
 * EXTENSIBLE_STRUCT; required prefix ends at json. Borrowed through configure return. NULL/zero
 * json means absent; otherwise bounded UTF-8 JSON. The plugin owns syntax and semantic validation.
 */
typedef struct rodaq_config {
    rodaq_struct_header_t header;
    rodaq_string_t json;
} rodaq_config_t;

/**
 * EXTENSIBLE_STRUCT; required prefix ends at log. Owned by Application/Framework for the valid
 * lifetime of the Plugin Instance. log may be NULL (treat as no-op). Must be thread-safe, bounded,
 * noexcept, and never reenter mutable lifecycle operations on the same plugin instance.
 */
typedef struct rodaq_application_callbacks {
    rodaq_struct_header_t header;
    void* context;
    void (*log)(void* context, rodaq_log_level_t level, rodaq_string_t message);
} rodaq_application_callbacks_t;

/**
 * EXTENSIBLE_STRUCT; required prefix ends at plugin_version. Immutable process-resident strings:
 * ID 1..255 canonical ASCII, name 1..255 UTF-8, version 1..128 strict SemVer 2.0.0 ASCII, no
 * embedded NUL. Manifest fields must match byte-for-byte before publish.
 */
typedef struct rodaq_plugin_descriptor {
    rodaq_struct_header_t header;

    rodaq_string_t plugin_id;
    rodaq_string_t plugin_name;
    rodaq_string_t plugin_version;
} rodaq_plugin_descriptor_t;

/**
 * FROZEN_VALUE. Instance ownership transfers only on successful create. Unique invalid value is
 * {NULL,NULL}; non-OK create must preserve it. Never guess cleanup from malformed output.
 */
typedef struct rodaq_plugin_instance {
    rodaq_plugin_context_t* context;
    const struct rodaq_plugin_instance_vtable* vtable;
} rodaq_plugin_instance_t;

/**
 * EXTENSIBLE_STRUCT; configure/query/shutdown/destroy are all required. All contexts and non-error
 * pointer parameters are non-NULL; error is optional. No C++ exception may cross these functions.
 * configure runs once and borrows config until return; application callbacks remain valid through
 * instance lifetime before destroy. On failure shutdown cleans partial resources. query is bounded,
 * read-only, SuccessOnly and requires Open plus call admission. shutdown accepts partial
 * configuration, stops instance-dependent work; OK proves destroy-safe, not domain success. destroy
 * runs at most once after shutdown OK and active calls drain, never concurrently with any instance
 * call; it cannot throw.
 */
typedef struct rodaq_plugin_instance_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*configure)(
        rodaq_plugin_context_t* context, const rodaq_application_callbacks_t* application,
        const rodaq_config_t* config, rodaq_error_t* error);

    rodaq_status_code_t (*query)(
        rodaq_plugin_context_t* context, const rodaq_interface_query_t* query,
        rodaq_interface_reference_t* interface, rodaq_error_t* error);

    rodaq_status_code_t (*shutdown)(
        rodaq_plugin_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);

    void (*destroy)(rodaq_plugin_context_t* context);
} rodaq_plugin_instance_vtable_t;

/**
 * EXTENSIBLE_STRUCT; descriptor and create are required, immutable and process-resident. descriptor
 * is validated before lifecycle calls. create initializes its non-NULL output to {NULL,NULL}, does
 * not consume config or start long-lived workers; error is optional.
 */
typedef struct rodaq_plugin_entry {
    rodaq_struct_header_t header;

    const rodaq_plugin_descriptor_t* (*descriptor)(void);

    rodaq_status_code_t (*create)(rodaq_plugin_instance_t* instance, rodaq_error_t* error);
} rodaq_plugin_entry_t;

#ifndef RODAQ_PLUGIN_EXPORT_SYMBOL
#if defined(__GNUC__) || defined(__clang__)
#define RODAQ_PLUGIN_EXPORT_SYMBOL __attribute__((visibility("default")))
#else
#define RODAQ_PLUGIN_EXPORT_SYMBOL
#endif
#endif

#define RODAQ_PLUGIN_ABI_VERSION RODAQ_PLUGIN_ABI_ENCODE(1, 0)

#define RODAQ_PLUGIN_ABI_ENCODE(major, minor) \
    ((((uint32_t)(major)) << 16u) | ((uint32_t)(minor)&0xffffu))

#define RODAQ_PLUGIN_ABI_MAJOR(value) ((uint16_t)(((uint32_t)(value)) >> 16u))

#define RODAQ_PLUGIN_ABI_MINOR(value) ((uint16_t)((uint32_t)(value)&0xffffu))

RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void);

RODAQ_PLUGIN_EXPORT_SYMBOL
const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void);

#ifdef __cplusplus
}
#endif

#endif
