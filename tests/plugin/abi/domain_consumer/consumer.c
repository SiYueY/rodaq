#include "domain_fixture.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct host_context {
    uint8_t copied[3];
    uint32_t ready;
    uint32_t submissions;
};

#if !DOMAIN_STORAGE
static rodaq_status_code_t submit_record(
    rodaq_interface_context_t* context, const rodaq_record_attempt_t* record,
    rodaq_submit_result_t* output) {
    struct host_context* host = (struct host_context*)context;
    assert(output->header.struct_size >= sizeof(rodaq_submit_result_t));
    assert(record->payload.logical_size == sizeof(host->copied));
    memcpy(host->copied, record->payload.data, sizeof(host->copied));
    ++host->submissions;
    output->disposition = RODAQ_SUBMIT_ACCEPTED;
    output->reserved0 = 0;
#if FIXTURE_MINOR > 0
    if (output->header.struct_size >= sizeof(domain_submission_t)) {
        ((domain_submission_t*)output)->optional_fact = 77;
    }
#endif
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t propose_descriptor(
    rodaq_interface_context_t* context, const rodaq_descriptor_proposal_t* proposal,
    rodaq_deadline_t deadline, rodaq_descriptor_token_t* token, rodaq_error_t* error) {
    (void)context;
    (void)deadline;
    if (error != NULL) {
        error->message.size = 0;
    }
    assert(proposal->stream_id == 42);
    *token = (rodaq_descriptor_token_t){21, 31};
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t report_source_event(
    rodaq_interface_context_t* context, const rodaq_source_event_t* event) {
    (void)context;
    (void)event;
    return RODAQ_STATUS_OK;
}

static rodaq_status_code_t report_acquisition_event(
    rodaq_interface_context_t* context, const rodaq_acquisition_event_t* event) {
    struct host_context* host = (struct host_context*)context;
    assert(event->kind == RODAQ_ACQUISITION_EVENT_STREAM_READY && event->stream_id == 42);
    host->ready = 1;
    return RODAQ_STATUS_OK;
}

static const domain_services_t kServices = {
    {sizeof(domain_services_t)},
    submit_record,
    propose_descriptor,
    report_source_event,
    report_acquisition_event
#if FIXTURE_MINOR > 0
    ,
    NULL /* Optional but byte-readable is a supported unavailable capability. */
#endif
};
#endif

static rodaq_status_code_t query_service(
    rodaq_service_registry_context_t* context, const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* output, rodaq_error_t* error) {
    if (error != NULL) {
        error->message.size = 0;
    }
    memset(output, 0, sizeof(*output));
#if DOMAIN_STORAGE
    (void)context;
    (void)query;
    return RODAQ_STATUS_NOT_FOUND;
#else
    if (query->major != 1 || query->min_minor > FIXTURE_MINOR ||
        query->interface_id.size != sizeof(RODAQ_ACQUISITION_SERVICE_ID) - 1 ||
        memcmp(query->interface_id.data, RODAQ_ACQUISITION_SERVICE_ID, query->interface_id.size) !=
            0) {
        return RODAQ_STATUS_UNAVAILABLE;
    }
    output->context = (rodaq_interface_context_t*)context;
    output->vtable = (const rodaq_interface_vtable_t*)&kServices;
    output->vtable_size = sizeof(kServices);
    output->negotiated_version = (rodaq_interface_version_t){
        1, query->max_minor > FIXTURE_MINOR ? FIXTURE_MINOR : query->max_minor};
    return RODAQ_STATUS_OK;
#endif
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const unsigned int provider_minor = (unsigned int)strtoul(argv[2], NULL, 10);
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    void* version_address = dlsym(library, "rodaq_plugin_abi_version");
    void* entry_address = dlsym(library, "rodaq_plugin_get_entry");
    uint32_t (*version)(void) = NULL;
    const rodaq_plugin_entry_t* (*get_entry)(void) = NULL;
    _Static_assert(sizeof(version) == sizeof(version_address), "POSIX function pointer size");
    memcpy(&version, &version_address, sizeof(version));
    memcpy(&get_entry, &entry_address, sizeof(get_entry));
    assert(version != NULL && get_entry != NULL && version() == RODAQ_PLUGIN_ABI_VERSION);
    const rodaq_plugin_entry_t* entry = get_entry();
    assert(entry != NULL && entry->create != NULL);
    rodaq_plugin_instance_t instance = {0};
    assert(entry->create(&instance, NULL) == RODAQ_STATUS_OK);
    struct host_context host = {{0}, 0, 0};
    const rodaq_service_registry_vtable_t registry = {
        {offsetof(rodaq_service_registry_vtable_t, query) + sizeof(registry.query)},
        query_service,
        NULL,
        NULL};
    const rodaq_service_registry_reference_t services = {
        (rodaq_service_registry_context_t*)&host, &registry};
    const rodaq_config_t config = {{sizeof(config)}, {NULL, 0}};
    assert(
        instance.vtable->configure(instance.context, &services, &config, NULL) == RODAQ_STATUS_OK);
    rodaq_interface_query_t request = {
        {DOMAIN_INTERFACE_ID, sizeof(DOMAIN_INTERFACE_ID) - 1}, 1, 0, FIXTURE_MINOR};
    rodaq_interface_reference_t reference = {0};
    assert(instance.vtable->query(instance.context, &request, &reference, NULL) == RODAQ_STATUS_OK);
    assert(reference.negotiated_version.major == 1);
    assert(
        reference.negotiated_version.minor ==
        (provider_minor > FIXTURE_MINOR ? FIXTURE_MINOR : provider_minor));
    assert(reference.vtable_size >= sizeof(rodaq_struct_header_t));
    const domain_operations_t* table = (const domain_operations_t*)reference.vtable;
    size_t readable = reference.vtable_size;
    if (readable > table->header.struct_size) {
        readable = table->header.struct_size;
    }
    if (readable > sizeof(domain_operations_t)) {
        readable = sizeof(domain_operations_t);
    }
    assert(readable >= DOMAIN_REQUIRED_PREFIX);
    const rodaq_deadline_t deadline = {RODAQ_DEADLINE_INFINITE};
#if DOMAIN_STORAGE
    const rodaq_storage_prepare_config_t prepare = {
        {sizeof(prepare)}, 8, 16, 2, 2, 100, 1024, 4096, 2, 31, 1, 1, 1};
    struct {
        domain_capacity_t value;
        uint8_t guard[16];
    } capacity = {0};
    capacity.value.header.struct_size = sizeof(capacity.value);
    memset(capacity.guard, 0xa5, sizeof(capacity.guard));
    assert(
        table->prepare(
            reference.context, &prepare, deadline, (rodaq_storage_capacity_model_t*)&capacity.value,
            NULL) == RODAQ_STATUS_OK);
    assert(capacity.value.header.struct_size == sizeof(capacity.value));
    assert(capacity.value.payload_expansion_denominator == 1);
    for (size_t i = 0; i < sizeof(capacity.guard); ++i) {
        assert(capacity.guard[i] == 0xa5);
    }
#if FIXTURE_MINOR > 0
    if (reference.negotiated_version.minor >= 1) {
        assert(capacity.value.optional_fact == 77);
    }
#endif
    rodaq_storage_descriptor_t descriptor = {0};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.descriptor_id = 5;
    descriptor.stream_id = 42;
    descriptor.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    descriptor.channel_name = (rodaq_string_t){"sample", 6};
    descriptor.message_encoding = (rodaq_string_t){"raw", 3};
    assert(
        table->prepare_descriptor(reference.context, &descriptor, deadline, NULL) ==
        RODAQ_STATUS_OK);
    const rodaq_segment_spec_t segment = {{sizeof(segment)}, 1, {"/unused/compat", 14}};
    assert(table->open_segment(reference.context, &segment, deadline, NULL) == RODAQ_STATUS_OK);
    uint8_t payload[3] = {3, 2, 1};
    rodaq_storage_record_t records[2];
    memset(records, 0, sizeof(records));
    for (size_t i = 0; i < 2; ++i) {
        records[i].header.struct_size = sizeof(records[i]);
        records[i].stream_id = 42;
        records[i].descriptor_id = 5;
        records[i].acceptance_sequence = i + 1;
        records[i].system_boot_id = 1;
        records[i].clock_epoch_id = 1;
        records[i].payload = (rodaq_bytes_t){payload, sizeof(payload)};
    }
    struct {
        domain_batch_result_t value;
        uint8_t guard[16];
    } batch = {0};
    batch.value.header.struct_size = sizeof(batch.value);
    memset(batch.guard, 0xa5, sizeof(batch.guard));
    assert(
        table->write_batch(
            reference.context, records, 2, deadline, (rodaq_storage_batch_result_t*)&batch.value,
            NULL) == RODAQ_STATUS_OK);
    assert(batch.value.confirmed_prefix == 2 && batch.value.tail_state == RODAQ_STORAGE_TAIL_NONE);
    assert(batch.value.header.struct_size == sizeof(batch.value));
    for (size_t i = 0; i < sizeof(batch.guard); ++i) {
        assert(batch.guard[i] == 0xa5);
    }
    payload[0] = 0xee;
#if FIXTURE_MINOR > 0
    if (reference.negotiated_version.minor >= 1 &&
        readable >= offsetof(domain_operations_t, optional_fact) + sizeof(table->optional_fact)) {
        uint64_t fact = 0;
        assert(
            table->optional_fact != NULL &&
            table->optional_fact(reference.context, &fact, NULL) == RODAQ_STATUS_OK && fact == 3);
        assert(batch.value.optional_fact == 77);
    }
#endif
    payload[0] = 3;
    assert(
        table->write_batch(
            reference.context, records, 2, deadline, (rodaq_storage_batch_result_t*)&batch.value,
            NULL) == RODAQ_STATUS_UNAVAILABLE);
    assert(
        batch.value.confirmed_prefix == 1 && batch.value.tail_state == RODAQ_STORAGE_TAIL_UNKNOWN);
    rodaq_segment_usage_t usage = {{sizeof(usage)}, 0, 0, 0, 0};
    assert(
        table->segment_usage(reference.context, &usage, NULL) == RODAQ_STATUS_OK &&
        usage.physical_bytes == 9);
    char path[32] = {0};
    char unused[32] = {0};
    rodaq_sealed_file_t files[2] = {{{path, sizeof(path), 0}, 0}, {{unused, sizeof(unused), 0}, 0}};
    rodaq_segment_seal_result_t sealed = {{sizeof(sealed)}, files, 2, 0, 0};
    assert(table->seal_segment(reference.context, deadline, &sealed, NULL) == RODAQ_STATUS_OK);
    assert(
        sealed.files == files && sealed.file_capacity == 2 && sealed.file_count == 1 &&
        sealed.total_physical_bytes == 9);
    assert(files[0].relative_path.data == path && files[0].relative_path.capacity == sizeof(path));
    assert(
        files[1].relative_path.data == unused && files[1].relative_path.capacity == sizeof(unused));
    assert(files[1].relative_path.size == 0 && files[1].physical_size == 0);
    assert(strcmp(path, "compat.bin") == 0);
#else
    const rodaq_acquisition_stream_binding_t stream = {{sizeof(stream)}, 42, {"sample", 6}, 8};
    const rodaq_acquisition_prepare_config_t prepare = {{sizeof(prepare)}, &stream, 1, 1};
    assert(table->prepare(reference.context, &prepare, deadline, NULL) == RODAQ_STATUS_OK);
    assert(table->start(reference.context, deadline, NULL) == RODAQ_STATUS_OK);
    assert(
        host.ready == 1 && host.submissions == 1 && host.copied[0] == 3 && host.copied[1] == 2 &&
        host.copied[2] == 1);
#if FIXTURE_MINOR > 0
    if (reference.negotiated_version.minor >= 1 &&
        readable >= offsetof(domain_operations_t, optional_fact) + sizeof(table->optional_fact)) {
        uint64_t fact = 0;
        assert(
            table->optional_fact != NULL &&
            table->optional_fact(reference.context, &fact, NULL) == RODAQ_STATUS_OK && fact == 1);
    }
#endif
#endif
    assert(table->quiesce(reference.context, deadline, NULL) == RODAQ_STATUS_OK);
    request.major = 2;
    assert(
        instance.vtable->query(instance.context, &request, &reference, NULL) ==
        RODAQ_STATUS_UNAVAILABLE);
    assert(reference.context == NULL && reference.vtable == NULL && reference.vtable_size == 0);
    request.major = 1;
    request.min_minor = 1;
    request.max_minor = 1;
    const rodaq_status_code_t required_minor =
        instance.vtable->query(instance.context, &request, &reference, NULL);
    assert(required_minor == (provider_minor == 0 ? RODAQ_STATUS_UNAVAILABLE : RODAQ_STATUS_OK));
    assert(instance.vtable->shutdown(instance.context, deadline, NULL) == RODAQ_STATUS_OK);
    instance.vtable->destroy(instance.context);
    /* Successful DSO is intentionally resident until this process exits. */
    return 0;
}
