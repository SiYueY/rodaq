#ifndef RODAQ_ACQUISITION_C_INTERFACE_H
#define RODAQ_ACQUISITION_C_INTERFACE_H

#include <rodaq/acquisition/c/types.h>
#include <rodaq/plugin/c/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_ACQUISITION_INTERFACE_ID "com.rodaq.acquisition"
#define RODAQ_ACQUISITION_INTERFACE_MAJOR 1u
#define RODAQ_ACQUISITION_INTERFACE_MINOR 0u

/**
 * EXTENSIBLE_STRUCT; all three slots required in major 1. Non-NULL context/config/callbacks,
 * optional error. prepare borrows bindings, binds immutable stream descriptors via callbacks and
 * establishes sources; start enables bounded callbacks, not Core Admission. quiesce is idempotent
 * and OK proves callbacks settled and callbacks table will no longer be accessed. Mutating
 * lifecycle calls are serialized; no callback may synchronously reenter them. Operation deadlines
 * do not cancel native work.
 */
typedef struct rodaq_acquisition_interface_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*prepare)(
        rodaq_interface_context_t* context, const rodaq_acquisition_prepare_config_t* config,
        const rodaq_acquisition_callbacks_t* callbacks, rodaq_deadline_t deadline,
        rodaq_error_t* error);

    rodaq_status_code_t (*start)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*quiesce)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);
} rodaq_acquisition_interface_vtable_t;

#ifdef __cplusplus
}
#endif

#endif
