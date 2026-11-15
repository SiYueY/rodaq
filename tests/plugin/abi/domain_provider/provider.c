#include "domain_fixture.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Fixed-memory ABI fixture; returned file facts do not represent real format I/O. */
struct rodaq_plugin_context {
    rodaq_service_registry_reference_t services;
    rodaq_interface_reference_t acquisition_service;
    uint64_t calls;
    uint64_t bytes;
    uint8_t copied[8];
};

static void clear_error(rodaq_error_t* error) {
    if (error != NULL) {
        error->message.size = 0;
    }
}

static rodaq_status_code_t quiesce(
    rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error) {
    (void)context;
    (void)deadline;
    clear_error(error);
    return RODAQ_STATUS_OK;
}

#if FIXTURE_MINOR > 0
static rodaq_status_code_t optional_fact(
    rodaq_interface_context_t* context, uint64_t* fact, rodaq_error_t* error) {
    clear_error(error);
    const struct rodaq_plugin_context* state = (const struct rodaq_plugin_context*)context;
    *fact = DOMAIN_STORAGE ? state->copied[0] : state->calls;
    return RODAQ_STATUS_OK;
}
#endif

#if DOMAIN_STORAGE
static rodaq_status_code_t prepare(
    rodaq_interface_context_t* context, const rodaq_storage_prepare_config_t* config,
    rodaq_deadline_t deadline, rodaq_storage_capacity_model_t* model, rodaq_error_t* error) {
    (void)context;
    (void)deadline;
    clear_error(error);
    if (config == NULL || model == NULL ||
        model->header.struct_size < sizeof(rodaq_storage_capacity_model_t)) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    model->payload_expansion_numerator = 1;
    model->payload_expansion_denominator = 1;
    model->per_record_overhead_bytes = 0;
    model->descriptor_expansion_numerator = 1;
    model->descriptor_expansion_denominator = 1;
    model->per_descriptor_overhead_bytes = 0;
    model->fixed_segment_overhead_bytes = 0;
    model->seal_reserve_bytes = 0;
#if FIXTURE_MINOR > 0
    if (model->header.struct_size >= sizeof(domain_capacity_t)) {
        ((domain_capacity_t*)model)->optional_fact = 77;
    }
#endif
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t prepare_descriptor(
    rodaq_interface_context_t* context, const rodaq_storage_descriptor_t* descriptor,
    rodaq_deadline_t deadline, rodaq_error_t* error) {
    (void)descriptor;
    return quiesce(context, deadline, error);
}

static rodaq_status_code_t open_segment(
    rodaq_interface_context_t* context, const rodaq_segment_spec_t* segment,
    rodaq_deadline_t deadline, rodaq_error_t* error) {
    (void)segment;
    return quiesce(context, deadline, error);
}

static rodaq_status_code_t write_batch(
    rodaq_interface_context_t* context, const rodaq_storage_record_t* records, uint32_t count,
    rodaq_deadline_t deadline, rodaq_storage_batch_result_t* output, rodaq_error_t* error) {
    (void)deadline;
    clear_error(error);
    struct rodaq_plugin_context* state = (struct rodaq_plugin_context*)context;
    assert(output->header.struct_size >= sizeof(rodaq_storage_batch_result_t));
    assert(count == 2 && records[0].payload.size == 3);
    memcpy(state->copied, records[0].payload.data, records[0].payload.size);
    ++state->calls;
    output->confirmed_prefix = state->calls == 1 ? count : 1;
    output->tail_state = state->calls == 1 ? RODAQ_STORAGE_TAIL_NONE : RODAQ_STORAGE_TAIL_UNKNOWN;
    state->bytes += output->confirmed_prefix * records[0].payload.size;
#if FIXTURE_MINOR > 0
    if (output->header.struct_size >= sizeof(domain_batch_result_t)) {
        ((domain_batch_result_t*)output)->optional_fact = 77;
    }
#endif
    return state->calls == 1 ? RODAQ_STATUS_OK : RODAQ_STATUS_UNAVAILABLE;
}

static rodaq_status_code_t segment_usage(
    rodaq_interface_context_t* context, rodaq_segment_usage_t* output, rodaq_error_t* error) {
    clear_error(error);
    assert(output->header.struct_size >= sizeof(rodaq_segment_usage_t));
    output->physical_bytes = ((const struct rodaq_plugin_context*)context)->bytes;
    output->pending_physical_upper_bound = 0;
    output->file_count = 1;
    output->reserved0 = 0;
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t seal_segment(
    rodaq_interface_context_t* context, rodaq_deadline_t deadline,
    rodaq_segment_seal_result_t* output, rodaq_error_t* error) {
    (void)deadline;
    clear_error(error);
    assert(output->header.struct_size >= sizeof(rodaq_segment_seal_result_t));
    assert(
        output->file_capacity >= 1 &&
        output->files[0].relative_path.capacity >= sizeof("compat.bin"));
    memcpy(output->files[0].relative_path.data, "compat.bin", sizeof("compat.bin"));
    output->files[0].relative_path.size = sizeof("compat.bin") - 1;
    output->files[0].physical_size = ((const struct rodaq_plugin_context*)context)->bytes;
    output->file_count = 1;
    output->total_physical_bytes = output->files[0].physical_size;
    return RODAQ_STATUS_OK;
}
#else
static rodaq_status_code_t prepare(
    rodaq_interface_context_t* context, const rodaq_acquisition_prepare_config_t* config,
    rodaq_deadline_t deadline, rodaq_error_t* error) {
    clear_error(error);
    struct rodaq_plugin_context* state = (struct rodaq_plugin_context*)context;
    assert(config->stream_count == 1 && config->streams[0].stream_id == 42);
    rodaq_interface_query_t query = {
        {RODAQ_ACQUISITION_SERVICE_ID, sizeof(RODAQ_ACQUISITION_SERVICE_ID) - 1},
        1,
        0,
        FIXTURE_MINOR};
    const rodaq_status_code_t code = state->services.vtable->query(
        state->services.context, &query, &state->acquisition_service, error);
    if (code != RODAQ_STATUS_OK) {
        return code;
    }
    const rodaq_acquisition_service_vtable_t* service =
        (const rodaq_acquisition_service_vtable_t*)state->acquisition_service.vtable;
    const size_t required = offsetof(rodaq_acquisition_service_vtable_t, report_acquisition_event) +
                            sizeof(service->report_acquisition_event);
    assert(
        state->acquisition_service.vtable_size >= required &&
        service->header.struct_size >= required);
#if FIXTURE_MINOR > 0
    if (state->acquisition_service.negotiated_version.minor >= 1 &&
        state->acquisition_service.vtable_size >= sizeof(domain_services_t) &&
        service->header.struct_size >= sizeof(domain_services_t)) {
        const domain_services_t* extended = (const domain_services_t*)service;
        assert(
            extended->optional_fact ==
            NULL); /* NULL optional tail does not invalidate the required prefix. */
    }
#endif
    rodaq_descriptor_proposal_t proposal = {0};
    proposal.header.struct_size = sizeof(proposal);
    proposal.stream_id = 42;
    proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    proposal.channel_name = (rodaq_string_t){"sample", 6};
    proposal.message_encoding = (rodaq_string_t){"raw", 3};
    rodaq_descriptor_token_t token = {0};
    assert(
        service->propose_descriptor(
            state->acquisition_service.context, &proposal, deadline, &token, error) ==
        RODAQ_STATUS_OK);
    assert(token.value == 21 && token.generation == 31);
    rodaq_acquisition_event_t event = {0};
    event.header.struct_size = sizeof(event);
    event.kind = RODAQ_ACQUISITION_EVENT_STREAM_READY;
    event.has_stream_id = 1;
    event.stream_id = 42;
    assert(
        service->report_acquisition_event(state->acquisition_service.context, &event) ==
        RODAQ_STATUS_OK);
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t start(
    rodaq_interface_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error) {
    (void)deadline;
    clear_error(error);
    struct rodaq_plugin_context* state = (struct rodaq_plugin_context*)context;
    const rodaq_acquisition_service_vtable_t* service =
        (const rodaq_acquisition_service_vtable_t*)state->acquisition_service.vtable;
    uint8_t bytes[3] = {3, 2, 1};
    rodaq_record_attempt_t record = {0};
    record.header.struct_size = sizeof(record);
    record.stream_id = 42;
    record.descriptor_token = (rodaq_descriptor_token_t){21, 31};
    record.payload = (rodaq_payload_t){{sizeof(record.payload)}, bytes, sizeof(bytes)};
    struct {
        domain_submission_t value;
        uint8_t guard[16];
    } output = {0};
    output.value.header.struct_size = sizeof(output.value);
    memset(output.guard, 0xa5, sizeof(output.guard));
    assert(
        service->submit_record(
            state->acquisition_service.context, &record, (rodaq_submit_result_t*)&output.value) ==
        RODAQ_STATUS_OK);
    assert(output.value.disposition == RODAQ_SUBMIT_ACCEPTED);
    assert(output.value.header.struct_size == sizeof(output.value));
    for (size_t i = 0; i < sizeof(output.guard); ++i) {
        assert(output.guard[i] == 0xa5);
    }
#if FIXTURE_MINOR > 0
    if (state->acquisition_service.negotiated_version.minor >= 1) {
        assert(output.value.optional_fact == 77);
    }
#endif
    memset(bytes, 0xee, sizeof(bytes));
    ++state->calls;
    return RODAQ_STATUS_OK;
}
#endif

static const domain_operations_t kOperations = {
    {sizeof(domain_operations_t)},
    prepare,
#if DOMAIN_STORAGE
    prepare_descriptor,
    open_segment,
    write_batch,
    segment_usage,
    seal_segment,
#else
    start,
#endif
    quiesce
#if FIXTURE_MINOR > 0
    ,
    optional_fact
#endif
};

static rodaq_status_code_t configure(
    rodaq_plugin_context_t* context, const rodaq_service_registry_reference_t* services,
    const rodaq_config_t* config, rodaq_error_t* error) {
    (void)config;
    clear_error(error);
    context->services = *services;
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t query(
    rodaq_plugin_context_t* context, const rodaq_interface_query_t* request,
    rodaq_interface_reference_t* output, rodaq_error_t* error) {
    clear_error(error);
    memset(output, 0, sizeof(*output));
    if (request->major != 1 || request->min_minor > FIXTURE_MINOR ||
        request->min_minor > request->max_minor ||
        request->interface_id.size != sizeof(DOMAIN_INTERFACE_ID) - 1 ||
        memcmp(request->interface_id.data, DOMAIN_INTERFACE_ID, sizeof(DOMAIN_INTERFACE_ID) - 1) !=
            0) {
        return RODAQ_STATUS_UNAVAILABLE;
    }
    output->context = (rodaq_interface_context_t*)context;
    output->vtable = (const rodaq_interface_vtable_t*)&kOperations;
    output->vtable_size = sizeof(kOperations);
    output->negotiated_version = (rodaq_interface_version_t){
        1, request->max_minor > FIXTURE_MINOR ? FIXTURE_MINOR : request->max_minor};
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t shutdown(
    rodaq_plugin_context_t* context, rodaq_deadline_t deadline, rodaq_error_t* error) {
    return quiesce((rodaq_interface_context_t*)context, deadline, error);
}

static void destroy(rodaq_plugin_context_t* context) { free(context); }

static const rodaq_plugin_instance_vtable_t kInstance = {
    {sizeof(kInstance)}, configure, query, shutdown, destroy};

rodaq_status_code_t rodaq_plugin_create_instance(
    rodaq_plugin_instance_t* output, rodaq_error_t* error) {
    clear_error(error);
    memset(output, 0, sizeof(*output));
    output->context = (rodaq_plugin_context_t*)calloc(1, sizeof(struct rodaq_plugin_context));
    if (output->context == NULL) {
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    }
    output->vtable = &kInstance;
    return RODAQ_STATUS_OK;
}
