#ifndef RODAQ_CONFIG_H
#define RODAQ_CONFIG_H

#include <rodaq/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t rodaq_config_kind;

#define RODAQ_CONFIG_NULL UINT32_C(0)
#define RODAQ_CONFIG_BOOL UINT32_C(1)
#define RODAQ_CONFIG_INT64 UINT32_C(2)
#define RODAQ_CONFIG_DOUBLE UINT32_C(3)
#define RODAQ_CONFIG_STRING UINT32_C(4)
#define RODAQ_CONFIG_MAP UINT32_C(5)
#define RODAQ_CONFIG_LIST UINT32_C(6)

#define RODAQ_CONFIG_PROTOCOL_VERSION UINT32_C(1)

typedef struct rodaq_config_view rodaq_config_view;

/* ABI-APPENDABLE: provider-owned read-only configuration table. */
typedef struct rodaq_config_vtable {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_config_kind (*kind)(void* context) RODAQ_NOEXCEPT;

    uint64_t (*size)(void* context) RODAQ_NOEXCEPT;

    rodaq_status (*find)(
        void* context, rodaq_string_view key, rodaq_config_view* out_child,
        rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*at)(
        void* context, uint64_t index, rodaq_config_view* out_child,
        rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*key_at)(
        void* context, uint64_t index, rodaq_string_view* out_key,
        rodaq_error* out_error) RODAQ_NOEXCEPT;

    rodaq_status (*as_bool)(void* context, rodaq_bool* out_value, rodaq_error* out_error)
        RODAQ_NOEXCEPT;

    rodaq_status (*as_int64)(void* context, int64_t* out_value, rodaq_error* out_error)
        RODAQ_NOEXCEPT;

    rodaq_status (*as_double)(void* context, double* out_value, rodaq_error* out_error)
        RODAQ_NOEXCEPT;

    rodaq_status (*as_string)(void* context, rodaq_string_view* out_value, rodaq_error* out_error)
        RODAQ_NOEXCEPT;
} rodaq_config_vtable;

/* ABI-FROZEN: borrowed configuration pair. */
struct rodaq_config_view {
    void* context;
    const rodaq_config_vtable* vtable;
};

#define RODAQ_CONFIG_VTABLE_MIN_STRUCT_SIZE     \
    (offsetof(rodaq_config_vtable, as_string) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_config_vtable, as_string))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_CONFIG_H */
