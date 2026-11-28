#ifndef RODAQ_FIXTURE_COUNTER_H
#define RODAQ_FIXTURE_COUNTER_H

#include <rodaq/c/plugin.h>

#define RODAQ_COUNTER_INTERFACE_ID "com.example.counter"

typedef struct rodaq_counter_vtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*increment)(
        rodaq_interface_context_t* context, int64_t delta, int64_t* value, rodaq_error_t* error);
    rodaq_status_code_t (*current)(
        rodaq_interface_context_t* context, int64_t* value, rodaq_error_t* error);
} rodaq_counter_vtable_t;

#endif
