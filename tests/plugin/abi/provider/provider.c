#include "counter.h"

#if FIXTURE_MINOR > 0
#include "counter_fixture.h"
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Single-threaded compatibility fixture, not a behavioral/production backend. */
struct rodaq_plugin_context {
    int64_t value;
    int configured;
};

static void clear_error(rodaq_error_t* error) {
    if (error != NULL) {
        error->message.size = 0;
    }
}

static rodaq_status_code_t increment(
    rodaq_interface_context_t* context, int64_t delta, int64_t* value, rodaq_error_t* error) {
    clear_error(error);
    if (value == NULL) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *value = 0;
    if (context == NULL) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    struct rodaq_plugin_context* state = (struct rodaq_plugin_context*)context;
    if ((delta > 0 && state->value > INT64_MAX - delta) ||
        (delta < 0 && state->value < INT64_MIN - delta)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    state->value += delta;
    *value = state->value;
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t current(
    rodaq_interface_context_t* context, int64_t* value, rodaq_error_t* error) {
    return increment(context, 0, value, error);
}

#if FIXTURE_MINOR > 0
static rodaq_status_code_t reset(rodaq_interface_context_t* context, rodaq_error_t* error) {
    clear_error(error);
    if (context == NULL) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    ((struct rodaq_plugin_context*)context)->value = 0;
    return RODAQ_STATUS_OK;
}

static const rodaq_counter_fixture_vtable_t counter = {
    {sizeof(rodaq_counter_fixture_vtable_t)}, increment, current, reset};
#else
static const rodaq_counter_vtable_t counter = {
    {sizeof(rodaq_counter_vtable_t)}, increment, current};
#endif

static rodaq_status_code_t configure(
    rodaq_plugin_context_t* context, const rodaq_application_callbacks_t* application,
    const rodaq_config_t* config, rodaq_error_t* error) {
    clear_error(error);
    if (context == NULL || application == NULL ||
        application->header.struct_size <
            offsetof(rodaq_application_callbacks_t, log) + sizeof(application->log) ||
        config == NULL ||
        config->header.struct_size < offsetof(rodaq_config_t, json) + sizeof(config->json)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    if (context->configured) {
        return RODAQ_STATUS_INVALID_STATE;
    }
    context->configured = 1;
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t query(
    rodaq_plugin_context_t* context, const rodaq_interface_query_t* request,
    rodaq_interface_reference_t* reference, rodaq_error_t* error) {
    clear_error(error);
    if (reference == NULL) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    memset(reference, 0, sizeof(*reference));
    if (context == NULL || request == NULL || request->interface_id.data == NULL ||
        request->interface_id.size == 0 || request->interface_id.size > 255 ||
        request->min_minor > request->max_minor) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    if (!context->configured) {
        return RODAQ_STATUS_INVALID_STATE;
    }
    const char* id = request->interface_id.data;
    for (uint32_t i = 0; i < request->interface_id.size; ++i) {
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '.' ||
              id[i] == '_' || id[i] == '-')) {
            return RODAQ_STATUS_INVALID_ARGUMENT;
        }
    }
    if (request->interface_id.size != sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1 ||
        memcmp(id, RODAQ_COUNTER_INTERFACE_ID, sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1) != 0) {
        return RODAQ_STATUS_NOT_FOUND;
    }
    if (request->major != 1 || request->min_minor > FIXTURE_MINOR) {
        return RODAQ_STATUS_UNAVAILABLE;
    }
    reference->context = (rodaq_interface_context_t*)context;
    reference->vtable = (const rodaq_interface_vtable_t*)&counter;
    reference->vtable_size = sizeof(counter);
    reference->negotiated_version.major = 1;
#if FIXTURE_MINOR > 0
    reference->negotiated_version.minor =
        request->max_minor < FIXTURE_MINOR ? request->max_minor : FIXTURE_MINOR;
#else
    reference->negotiated_version.minor = 0;
#endif
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t shutdown(
    rodaq_plugin_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error) {
    clear_error(error);
    if (context == NULL || deadline.monotonic_deadline < 0) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    context->configured = 0;
    return RODAQ_STATUS_OK;
}

static void destroy(rodaq_plugin_context_t* context) { free(context); }

static const rodaq_plugin_instance_vtable_t lifecycle = {
    {sizeof(rodaq_plugin_instance_vtable_t)}, configure, query, shutdown, destroy};

rodaq_status_code_t rodaq_plugin_create_instance(
    rodaq_plugin_instance_t* instance, rodaq_error_t* error) {
    clear_error(error);
    if (instance == NULL) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    memset(instance, 0, sizeof(*instance));
    rodaq_plugin_context_t* state = calloc(1, sizeof(*state));
    if (state == NULL) {
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    }
    instance->context = state;
    instance->vtable = &lifecycle;
    return RODAQ_STATUS_OK;
}
