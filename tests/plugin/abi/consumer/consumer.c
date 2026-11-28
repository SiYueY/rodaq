#include "counter.h"

#if FIXTURE_MINOR > 0
#include "counter_fixture.h"
#endif

#include <assert.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    assert(argc == 2);
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
    _Static_assert(sizeof(get_entry) == sizeof(entry_address), "POSIX function pointer size");
    memcpy(&version, &version_address, sizeof(version));
    memcpy(&get_entry, &entry_address, sizeof(get_entry));
    assert(version != NULL && get_entry != NULL);
    assert(version() == RODAQ_PLUGIN_ABI_VERSION);
    const rodaq_plugin_entry_t* entry = get_entry();
    assert(entry != NULL);
    assert(
        entry->header.struct_size >=
        offsetof(rodaq_plugin_entry_t, create) + sizeof(entry->create));
    assert(entry->descriptor != NULL && entry->create != NULL);
    assert(entry->descriptor()->plugin_id.size == sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1);

    rodaq_plugin_instance_t instance = {0};
    assert(entry->create(&instance, NULL) == RODAQ_STATUS_OK);
    assert(instance.context != NULL && instance.vtable != NULL);
    assert(
        instance.vtable->header.struct_size >=
        offsetof(rodaq_plugin_instance_vtable_t, destroy) + sizeof(instance.vtable->destroy));
    const rodaq_application_callbacks_t application = {{sizeof(application)}, NULL, NULL};
    const rodaq_config_t config = {{sizeof(rodaq_config_t)}, {NULL, 0}};
    assert(
        instance.vtable->configure(instance.context, &application, &config, NULL) ==
        RODAQ_STATUS_OK);

    rodaq_interface_query_t request = {
        {RODAQ_COUNTER_INTERFACE_ID, sizeof(RODAQ_COUNTER_INTERFACE_ID) - 1}, 1, 0, FIXTURE_MINOR};
    rodaq_interface_reference_t reference = {0};
    assert(instance.vtable->query(instance.context, &request, &reference, NULL) == RODAQ_STATUS_OK);
    assert(reference.context != NULL && reference.vtable != NULL);
    assert(
        reference.negotiated_version.major == 1 &&
        reference.negotiated_version.minor <= FIXTURE_MINOR);
    assert(reference.vtable_size >= sizeof(rodaq_struct_header_t));
    const rodaq_counter_vtable_t* counter = (const rodaq_counter_vtable_t*)reference.vtable;
    size_t readable = reference.vtable_size < counter->header.struct_size
                          ? reference.vtable_size
                          : counter->header.struct_size;
    assert(readable >= offsetof(rodaq_counter_vtable_t, current) + sizeof(counter->current));
    assert(counter->increment != NULL && counter->current != NULL);
    int64_t value = 0;
    assert(counter->increment(reference.context, 7, &value, NULL) == RODAQ_STATUS_OK);
    assert(value == 7);
    assert(counter->current(reference.context, &value, NULL) == RODAQ_STATUS_OK && value == 7);

#if FIXTURE_MINOR > 0
    if (reference.negotiated_version.minor >= 1 &&
        readable >= offsetof(rodaq_counter_fixture_vtable_t, reset) +
                        sizeof(((rodaq_counter_fixture_vtable_t*)0)->reset)) {
        const rodaq_counter_fixture_vtable_t* extended =
            (const rodaq_counter_fixture_vtable_t*)reference.vtable;
        assert(extended->reset != NULL);
        assert(extended->reset(reference.context, NULL) == RODAQ_STATUS_OK);
        assert(counter->current(reference.context, &value, NULL) == RODAQ_STATUS_OK && value == 0);
    }
#endif

    request.major = 2;
    assert(
        instance.vtable->query(instance.context, &request, &reference, NULL) ==
        RODAQ_STATUS_UNAVAILABLE);
    assert(reference.context == NULL && reference.vtable == NULL && reference.vtable_size == 0);
    assert(reference.negotiated_version.major == 0 && reference.negotiated_version.minor == 0);
    assert(
        instance.vtable->shutdown(
            instance.context, (rodaq_deadline_t){RODAQ_DEADLINE_INFINITE}, NULL) ==
        RODAQ_STATUS_OK);
    instance.vtable->destroy(instance.context);
    /* Successful DSO stays resident; this independent consumer exits now. */
    return 0;
}
