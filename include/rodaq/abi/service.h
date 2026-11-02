#ifndef RODAQ_SERVICE_H
#define RODAQ_SERVICE_H

#include <rodaq/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_SERVICE_REGISTRY_PROTOCOL_VERSION UINT32_C(1)

/* ABI-APPENDABLE: provider-owned Service Registry table. */
typedef struct rodaq_service_registry_vtable {
    uint32_t protocol_version;
    uint32_t struct_size;

    rodaq_status (*query)(
        void* context, rodaq_string_view service_id, rodaq_interface* out_service,
        rodaq_error* out_error) RODAQ_NOEXCEPT;
} rodaq_service_registry_vtable;

/* ABI-FROZEN: borrowed context/vtable pair. */
typedef struct rodaq_service_registry {
    void* context;
    const rodaq_service_registry_vtable* vtable;
} rodaq_service_registry;

#define RODAQ_SERVICE_REGISTRY_VTABLE_MIN_STRUCT_SIZE \
    (offsetof(rodaq_service_registry_vtable, query) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_service_registry_vtable, query))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_SERVICE_H */
