#ifndef RODAQ_ACQUISITION_C_INTERFACE_H
#define RODAQ_ACQUISITION_C_INTERFACE_H

#include <rodaq/plugin/c/plugin.h>
#include <rodaq/acquisition/c/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_ACQUISITION_INTERFACE_ID "com.rodaq.acquisition"
#define RODAQ_ACQUISITION_INTERFACE_MAJOR 1u
#define RODAQ_ACQUISITION_INTERFACE_MINOR 0u

/**
 * EXTENSIBLE_STRUCT; all three slots required in major 1. Non-NULL context/config, optional error.
 * prepare borrows bindings and establishes sources; start enables bounded callbacks, not Core
 * Admission. quiesce is idempotent and OK proves callbacks settled. Mutating lifecycle calls are
 * serialized; no callback may synchronously reenter them. Operation deadlines do not cancel native
 * work.
 */
typedef struct rodaq_acquisition_interface_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*prepare)(
        rodaq_interface_context_t* context, const rodaq_acquisition_prepare_config_t* config,
        rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*start)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*quiesce)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);
} rodaq_acquisition_interface_vtable_t;

#define RODAQ_ACQUISITION_SERVICE_ID "com.rodaq.acquisition.service"
#define RODAQ_ACQUISITION_SERVICE_MAJOR 1u
#define RODAQ_ACQUISITION_SERVICE_MINOR 1u

/**
 * EXTENSIBLE_STRUCT; all four slots required. Non-NULL contexts and input/output pointers.
 * submit_record is bounded, no I/O, SuccessOnly (disposition separates rejection from callback
 * error). propose_descriptor is SuccessOnly with zero token on failure and may block outside hot
 * callback locks. Reports are bounded and nonblocking apart from short state locks. Async calls
 * require Registry managed-call permits; no synchronous acquisition control reentry.
 */
typedef struct rodaq_acquisition_service_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*submit_record)(
        rodaq_interface_context_t* context, const rodaq_record_attempt_t* record,
        rodaq_submit_result_t* submission);

    rodaq_status_code_t (*propose_descriptor)(
        rodaq_interface_context_t* context, const rodaq_descriptor_proposal_t* proposal,
        rodaq_deadline_t deadline, rodaq_descriptor_token_t* descriptor_token,
        rodaq_error_t* error);

    rodaq_status_code_t (*report_source_event)(
        rodaq_interface_context_t* context, const rodaq_source_event_t* event);

    rodaq_status_code_t (*report_acquisition_event)(
        rodaq_interface_context_t* context, const rodaq_acquisition_event_t* event);
} rodaq_acquisition_service_vtable_t;

#ifdef __cplusplus
}
#endif

#endif
