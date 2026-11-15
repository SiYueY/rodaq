#ifndef RODAQ_TEST_DOMAIN_FIXTURE_H
#define RODAQ_TEST_DOMAIN_FIXTURE_H

#include <rodaq/acquisition/c/interface.h>
#include <rodaq/storage/c/interface.h>

#include <stddef.h>

/* Synthetic 1.1 types belong only to compatibility fixtures, never production. */
#if DOMAIN_STORAGE
#define DOMAIN_INTERFACE_ID RODAQ_STORAGE_INTERFACE_ID
#define DOMAIN_REQUIRED_PREFIX                             \
    (offsetof(rodaq_storage_interface_vtable_t, quiesce) + \
     sizeof(((rodaq_storage_interface_vtable_t*)0)->quiesce))
#if FIXTURE_MINOR > 0
typedef struct domain_operations {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*prepare)(
        rodaq_interface_context_t*, const rodaq_storage_prepare_config_t*, rodaq_deadline_t,
        rodaq_storage_capacity_model_t*, rodaq_error_t*);
    rodaq_status_code_t (*prepare_descriptor)(
        rodaq_interface_context_t*, const rodaq_storage_descriptor_t*, rodaq_deadline_t,
        rodaq_error_t*);
    rodaq_status_code_t (*open_segment)(
        rodaq_interface_context_t*, const rodaq_segment_spec_t*, rodaq_deadline_t, rodaq_error_t*);
    rodaq_status_code_t (*write_batch)(
        rodaq_interface_context_t*, const rodaq_storage_record_t*, uint32_t, rodaq_deadline_t,
        rodaq_storage_batch_result_t*, rodaq_error_t*);
    rodaq_status_code_t (*segment_usage)(
        rodaq_interface_context_t*, rodaq_segment_usage_t*, rodaq_error_t*);
    rodaq_status_code_t (*seal_segment)(
        rodaq_interface_context_t*, rodaq_deadline_t, rodaq_segment_seal_result_t*, rodaq_error_t*);
    rodaq_status_code_t (*quiesce)(rodaq_interface_context_t*, rodaq_deadline_t, rodaq_error_t*);
    rodaq_status_code_t (*optional_fact)(rodaq_interface_context_t*, uint64_t*, rodaq_error_t*);
} domain_operations_t;

typedef struct domain_capacity {
    rodaq_struct_header_t header;
    uint64_t payload_expansion_numerator;
    uint64_t payload_expansion_denominator;
    uint64_t per_record_overhead_bytes;
    uint64_t descriptor_expansion_numerator;
    uint64_t descriptor_expansion_denominator;
    uint64_t per_descriptor_overhead_bytes;
    uint64_t fixed_segment_overhead_bytes;
    uint64_t seal_reserve_bytes;
    uint64_t optional_fact;
} domain_capacity_t;

typedef struct domain_batch_result {
    rodaq_struct_header_t header;
    uint32_t confirmed_prefix;
    rodaq_storage_tail_state_t tail_state;
    uint32_t optional_fact;
} domain_batch_result_t;

_Static_assert(
    offsetof(domain_capacity_t, optional_fact) >= sizeof(rodaq_storage_capacity_model_t),
    "Capacity tail only");
_Static_assert(
    offsetof(domain_batch_result_t, optional_fact) >= sizeof(rodaq_storage_batch_result_t),
    "BatchResult tail only");
#else
typedef rodaq_storage_interface_vtable_t domain_operations_t;
typedef rodaq_storage_capacity_model_t domain_capacity_t;
typedef rodaq_storage_batch_result_t domain_batch_result_t;
#endif
#else
#define DOMAIN_INTERFACE_ID RODAQ_ACQUISITION_INTERFACE_ID
#define DOMAIN_REQUIRED_PREFIX                                 \
    (offsetof(rodaq_acquisition_interface_vtable_t, quiesce) + \
     sizeof(((rodaq_acquisition_interface_vtable_t*)0)->quiesce))
#if FIXTURE_MINOR > 0
typedef struct domain_operations {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*prepare)(
        rodaq_interface_context_t*, const rodaq_acquisition_prepare_config_t*, rodaq_deadline_t,
        rodaq_error_t*);
    rodaq_status_code_t (*start)(rodaq_interface_context_t*, rodaq_deadline_t, rodaq_error_t*);
    rodaq_status_code_t (*quiesce)(rodaq_interface_context_t*, rodaq_deadline_t, rodaq_error_t*);
    rodaq_status_code_t (*optional_fact)(rodaq_interface_context_t*, uint64_t*, rodaq_error_t*);
} domain_operations_t;

typedef struct domain_services {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*submit_record)(
        rodaq_interface_context_t*, const rodaq_record_attempt_t*, rodaq_submit_result_t*);
    rodaq_status_code_t (*propose_descriptor)(
        rodaq_interface_context_t*, const rodaq_descriptor_proposal_t*, rodaq_deadline_t,
        rodaq_descriptor_token_t*, rodaq_error_t*);
    rodaq_status_code_t (*report_source_event)(
        rodaq_interface_context_t*, const rodaq_source_event_t*);
    rodaq_status_code_t (*report_acquisition_event)(
        rodaq_interface_context_t*, const rodaq_acquisition_event_t*);
    rodaq_status_code_t (*optional_fact)(rodaq_interface_context_t*, uint64_t*, rodaq_error_t*);
} domain_services_t;

typedef struct domain_submission {
    rodaq_struct_header_t header;
    rodaq_submit_disposition_t disposition;
    uint32_t reserved0;
    uint32_t optional_fact;
} domain_submission_t;
_Static_assert(
    offsetof(domain_submission_t, optional_fact) >= sizeof(rodaq_submit_result_t),
    "Submission tail only");
_Static_assert(
    offsetof(domain_services_t, optional_fact) >= sizeof(rodaq_acquisition_service_vtable_t),
    "Service tail only");
#else
typedef rodaq_acquisition_interface_vtable_t domain_operations_t;
typedef rodaq_acquisition_service_vtable_t domain_services_t;
typedef rodaq_submit_result_t domain_submission_t;
#endif
#endif

#if FIXTURE_MINOR > 0
_Static_assert(
    offsetof(domain_operations_t, optional_fact) >= DOMAIN_REQUIRED_PREFIX, "Operation tail only");
#endif
#endif
