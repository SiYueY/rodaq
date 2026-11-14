#ifndef RODAQ_STORAGE_C_INTERFACE_H
#define RODAQ_STORAGE_C_INTERFACE_H

#include <rodaq/plugin/c/plugin.h>
#include <rodaq/storage/c/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RODAQ_STORAGE_INTERFACE_ID "com.rodaq.storage"
#define RODAQ_STORAGE_INTERFACE_MAJOR 1u
#define RODAQ_STORAGE_INTERFACE_MINOR 0u

/**
 * EXTENSIBLE_STRUCT; all seven slots required in major 1. Non-NULL context/inputs/outputs; error
 * optional. One serialized mutable lane per instance, including late-return calls; never start a
 * second mutation after observer timeout. All deadlines are operation deadlines, segment_usage is
 * bounded/no I/O. write_batch requires nonempty aligned array; ValidOutcome after initial
 * validation, other outputs SuccessOnly. quiesce is idempotent and establishes no outstanding
 * writer/payload access; shutdown cannot invent successful seal or durable commit.
 */
typedef struct rodaq_storage_interface_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*prepare)(
        rodaq_interface_context_t* context, const rodaq_storage_prepare_config_t* config,
        rodaq_deadline_t deadline, rodaq_storage_capacity_model_t* capacity_model,
        rodaq_error_t* error);

    rodaq_status_code_t (*prepare_descriptor)(
        rodaq_interface_context_t* context, const rodaq_storage_descriptor_t* descriptor,
        rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*open_segment)(
        rodaq_interface_context_t* context, const rodaq_segment_spec_t* segment,
        rodaq_deadline_t deadline, rodaq_error_t* error);

    rodaq_status_code_t (*write_batch)(
        rodaq_interface_context_t* context, const rodaq_storage_record_t* records,
        uint32_t record_count, rodaq_deadline_t deadline,
        rodaq_storage_batch_result_t* batch_result, rodaq_error_t* error);

    rodaq_status_code_t (*segment_usage)(
        rodaq_interface_context_t* context, rodaq_segment_usage_t* usage, rodaq_error_t* error);

    rodaq_status_code_t (*seal_segment)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline,
        rodaq_segment_seal_result_t* seal_result, rodaq_error_t* error);

    rodaq_status_code_t (*quiesce)(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error);
} rodaq_storage_interface_vtable_t;
#ifdef __cplusplus
}
#endif

#endif
