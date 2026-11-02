#include <rodaq/abi/runtime.h>

namespace {

#define RODAQ_TEST_VIEW(value) \
    { value, sizeof(value) - 1 }

bool string_equals(rodaq_string_view value, const char* expected, uint64_t expected_size) {
    if (value.size != expected_size || value.data == nullptr) {
        return false;
    }

    for (uint64_t index = 0; index < expected_size; ++index) {
        if (value.data[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

rodaq_status create_instance(void** out_instance, rodaq_error*) noexcept {
    if (out_instance == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_instance = nullptr;
    return RODAQ_STATUS_OK;
}

void destroy_instance(void*) noexcept {}

rodaq_status bind_instance(void*, const rodaq_service_registry*, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status configure_instance(void*, rodaq_config_view, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status start_instance(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status stop_instance(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status prepare_binding_switch(
    void*, rodaq_binding_transition, rodaq_stream_handle, rodaq_stream_descriptor_handle,
    rodaq_stream_descriptor_handle, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status commit_binding_switch(void*, rodaq_binding_transition, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status abort_binding_switch(
    void*, rodaq_binding_transition, rodaq_binding_abort_disposition, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status quiesce_stream(void*, rodaq_stream_handle, rodaq_error*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status quiesce_plugin(void*, rodaq_error*) noexcept { return RODAQ_STATUS_OK; }

const rodaq_acquisition_control_v1 control = {
    RODAQ_ACQUISITION_CONTROL_PROTOCOL_VERSION,
    sizeof(rodaq_acquisition_control_v1),
    prepare_binding_switch,
    commit_binding_switch,
    abort_binding_switch,
    quiesce_stream,
    quiesce_plugin,
};

rodaq_status get_extension(
    void* instance, rodaq_string_view extension_id, rodaq_interface* out_extension,
    rodaq_error*) noexcept {
    if (out_extension == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    out_extension->context = nullptr;
    out_extension->vtable = nullptr;

    const char expected_id[] = RODAQ_ACQUISITION_CONTROL_EXTENSION_ID;
    if (!string_equals(extension_id, expected_id, sizeof(expected_id) - 1)) {
        return RODAQ_STATUS_NOT_FOUND;
    }

    out_extension->context = instance;
    out_extension->vtable = &control;
    return RODAQ_STATUS_OK;
}

const rodaq_plugin_descriptor descriptor = {
    sizeof(rodaq_plugin_descriptor),
    0,
    RODAQ_TEST_VIEW("rodaq.acquisition.symbol_fixture"),
    RODAQ_TEST_VIEW("Acquisition Symbol Fixture"),
    RODAQ_TEST_VIEW("1.0.0"),
    RODAQ_TEST_VIEW("M0-B symbol qualification fixture"),
};

const rodaq_acquisition_plugin plugin = {
    RODAQ_ACQUISITION_PROTOCOL_VERSION,
    sizeof(rodaq_acquisition_plugin),
    &descriptor,
    create_instance,
    destroy_instance,
    bind_instance,
    configure_instance,
    start_instance,
    stop_instance,
    get_extension,
};

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT const uint32_t rodaq_plugin_abi_version = RODAQ_PLUGIN_ABI_VERSION;

extern "C" RODAQ_PLUGIN_EXPORT const rodaq_acquisition_plugin* rodaq_get_acquisition_plugin(
    void) noexcept {
    return &plugin;
}
