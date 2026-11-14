#ifndef RODAQ_FIXTURE_COUNTER_MINOR1_H
#define RODAQ_FIXTURE_COUNTER_MINOR1_H

#include "counter.h"

/* Synthetic minor 1 tail is fixture-only, never part of the production ABI. */
typedef struct rodaq_counter_fixture_vtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*increment)(
        rodaq_interface_context_t* context, int64_t delta, int64_t* value, rodaq_error_t* error);
    rodaq_status_code_t (*current)(
        rodaq_interface_context_t* context, int64_t* value, rodaq_error_t* error);
    rodaq_status_code_t (*reset)(rodaq_interface_context_t* context, rodaq_error_t* error);
} rodaq_counter_fixture_vtable_t;

#endif
