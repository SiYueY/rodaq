#ifndef RODAQ_PLUGIN_H
#define RODAQ_PLUGIN_H

#include <rodaq/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI-APPENDABLE: provider-owned Plugin metadata. */
typedef struct rodaq_plugin_descriptor {
    uint32_t struct_size;
    uint32_t reserved;

    rodaq_string_view id;
    rodaq_string_view name;
    rodaq_string_view version;
    rodaq_string_view description;
} rodaq_plugin_descriptor;

typedef struct rodaq_acquisition_plugin rodaq_acquisition_plugin;
typedef struct rodaq_storage_plugin rodaq_storage_plugin;

extern RODAQ_PLUGIN_EXPORT const uint32_t rodaq_plugin_abi_version;

RODAQ_PLUGIN_EXPORT const rodaq_acquisition_plugin* rodaq_get_acquisition_plugin(void)
    RODAQ_NOEXCEPT;

RODAQ_PLUGIN_EXPORT const rodaq_storage_plugin* rodaq_get_storage_plugin(void) RODAQ_NOEXCEPT;

#define RODAQ_PLUGIN_DESCRIPTOR_MIN_STRUCT_SIZE       \
    (offsetof(rodaq_plugin_descriptor, description) + \
     RODAQ_ABI_FIELD_SIZE(rodaq_plugin_descriptor, description))

#ifdef __cplusplus
}
#endif

#endif /* RODAQ_PLUGIN_H */
