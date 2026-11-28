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
static rodaq_status_code_t prepare_descriptor(
    void* context, rodaq_stream_id_t stream_id, const rodaq_acquisition_descriptor_t* descriptor,
    rodaq_descriptor_id_t* descriptor_id, rodaq_error_t* error) {
    (void)context;
    if (error != NULL) {
        error->message.size = 0;
    }
    assert(stream_id == 42 && descriptor->stream_id == 42);
    *descriptor_id = 21;
    return RODAQ_STATUS_OK;
}

static rodaq_record_disposition_t submit_record(
    void* context, const rodaq_acquisition_record_attempt_t* record) {
    struct host_context* host = (struct host_context*)context;
    assert(record->payload.size == sizeof(host->copied));
    memcpy(host->copied, record->payload.data, sizeof(host->copied));
    ++host->submissions;
    return RODAQ_SUBMIT_ACCEPTED;
}

static void report_event(void* context, const rodaq_acquisition_event_t* event) {
    struct host_context* host = (struct host_context*)context;
    assert(event->kind == RODAQ_ACQUISITION_EVENT_STREAM_READY && event->stream_id == 42);
    host->ready = 1;
}
#endif

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
    const rodaq_application_callbacks_t application = {{sizeof(application)}, NULL, NULL};
    const rodaq_config_t config = {{sizeof(config)}, {NULL, 0}};
    assert(
        instance.vtable->configure(instance.context, &application, &config, NULL) ==
        RODAQ_STATUS_OK);
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
        {sizeof(prepare)}, 8, 16, 2, 2, 100, 1024, 4096, 2, 31};
    assert(table->prepare(reference.context, &prepare, deadline, NULL) == RODAQ_STATUS_OK);
    rodaq_storage_descriptor_t descriptor = {0};
    descriptor.header.struct_size = sizeof(descriptor);
    descriptor.descriptor_id = 5;
    descriptor.stream_id = 42;
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
        records[i].recording_time = 1000 + i;
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
    rodaq_segment_usage_t usage = {{sizeof(usage)}, 0, 0, 0};
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
    struct host_context host = {{0}, 0, 0};
    const rodaq_acquisition_callbacks_t callbacks = {
        {sizeof(callbacks)}, &host, prepare_descriptor, submit_record, report_event};
    const rodaq_acquisition_stream_binding_t stream = {{sizeof(stream)}, 42, {"sample", 6}, 8};
    const rodaq_acquisition_prepare_config_t prepare = {{sizeof(prepare)}, &stream, 1, 1};
    assert(
        table->prepare(reference.context, &prepare, &callbacks, deadline, NULL) == RODAQ_STATUS_OK);
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
    return 0;
}
