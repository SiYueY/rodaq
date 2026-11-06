#ifndef RODAQ_TEST_CURRENT_ABI_V1_BASELINE_ABI_H_
#define RODAQ_TEST_CURRENT_ABI_V1_BASELINE_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t rodaq_status_code;
#define RODAQ_STATUS_OK 0U
#define RODAQ_STATUS_NOT_FOUND 3U

typedef struct {
    const char* data;
    uint64_t size;
} rodaq_string_view;

typedef struct {
    const uint8_t* data;
    uint64_t size;
} rodaq_bytes_view;

typedef struct {
    uint64_t value;
} rodaq_stream_descriptor_handle_frozen;

typedef struct {
    uint64_t value;
} rodaq_clock_handle_frozen;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    rodaq_stream_descriptor_handle_frozen descriptor;
    rodaq_string_view projection_id;
    rodaq_bytes_view payload;
} rodaq_projection_view_v1_frozen;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    rodaq_clock_handle_frozen clock;
    int64_t clock_reading;
    uint64_t unix_anchor_ns;
} rodaq_unix_epoch_clock_anchor_v1_frozen;

typedef struct rodaq_status rodaq_status;
typedef struct rodaq_service_registry rodaq_service_registry;

typedef struct {
    void* context;
    const void* vtable;
} rodaq_interface;

typedef struct rodaq_config_vtable rodaq_config_vtable;

typedef struct {
    uint32_t struct_size;
    uint32_t reserved;
    rodaq_string_view id;
    rodaq_string_view name;
    rodaq_string_view version;
    rodaq_string_view description;
} rodaq_plugin_descriptor;

typedef struct {
    void* context;
    const rodaq_config_vtable* vtable;
} rodaq_config_view;

typedef struct rodaq_acquisition_control_v1 {
    uint32_t protocol_version;
    uint32_t struct_size;
    rodaq_status_code (*prepare_binding_switch)(
        void*, uint64_t, uint64_t, uint64_t, uint64_t, rodaq_status*);
    rodaq_status_code (*commit_binding_switch)(void*, uint64_t, rodaq_status*);
    rodaq_status_code (*abort_binding_switch)(void*, uint64_t, uint32_t, rodaq_status*);
    rodaq_status_code (*quiesce_stream)(void*, uint64_t, rodaq_status*);
    rodaq_status_code (*quiesce_plugin)(void*, rodaq_status*);
} rodaq_acquisition_control_v1;

typedef struct rodaq_acquisition_plugin {
    uint32_t protocol_version;
    uint32_t struct_size;
    const rodaq_plugin_descriptor* descriptor;
    rodaq_status_code (*create)(void**, rodaq_status*);
    void (*destroy)(void*);
    rodaq_status_code (*bind)(void*, const rodaq_service_registry*, rodaq_status*);
    rodaq_status_code (*configure)(void*, rodaq_config_view, rodaq_status*);
    rodaq_status_code (*start)(void*, rodaq_status*);
    rodaq_status_code (*stop)(void*, rodaq_status*);
    rodaq_status_code (*query_interface)(void*, rodaq_string_view, rodaq_interface*, rodaq_status*);
} rodaq_acquisition_plugin;

extern const uint32_t rodaq_plugin_abi_version;

#ifdef __cplusplus
}
#endif

#endif
