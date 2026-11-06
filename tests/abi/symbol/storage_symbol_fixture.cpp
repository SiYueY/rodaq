#include <rodaq/plugin/abi/runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

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

rodaq_status_code create_instance(void** out_instance, rodaq_status*) noexcept {
    if (out_instance == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_instance = nullptr;
    return RODAQ_STATUS_OK;
}

void destroy_instance(void*) noexcept {}

rodaq_status_code bind_instance(void*, const rodaq_service_registry*, rodaq_status*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status_code configure_instance(void*, rodaq_config_view, rodaq_status*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status_code open_storage(void*, const rodaq_storage_open_request*, rodaq_status*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status_code submit_record(
    void*, const rodaq_storage_submission*, rodaq_storage_submit_result*, rodaq_status*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status_code flush_storage(void*, rodaq_status*) noexcept { return RODAQ_STATUS_OK; }

rodaq_status_code close_storage(void*, const rodaq_storage_close_request*, rodaq_status*) noexcept {
    return RODAQ_STATUS_OK;
}

rodaq_status_code get_capabilities(
    void*, rodaq_storage_capabilities* out_capabilities, rodaq_status*) noexcept {
    if (out_capabilities == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    *out_capabilities = {};
    return RODAQ_STATUS_OK;
}

rodaq_status_code ensure_descriptor_ready(
    void*, rodaq_stream_descriptor_handle, rodaq_descriptor_readiness_result* out_result,
    rodaq_status*) noexcept {
    if (out_result == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint32_t capacity = out_result->struct_size;
    const std::uint32_t initialized = static_cast<std::uint32_t>(
        std::min<std::size_t>(capacity, sizeof(rodaq_descriptor_readiness_result)));
    if (initialized < RODAQ_DESCRIPTOR_READINESS_RESULT_MIN_STRUCT_SIZE) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    std::memset(out_result, 0, initialized);
    out_result->struct_size = initialized;
    out_result->state = RODAQ_DESCRIPTOR_READY;
    return RODAQ_STATUS_OK;
}

const rodaq_storage_control control = {
    RODAQ_STORAGE_CONTROL_PROTOCOL_VERSION,
    sizeof(rodaq_storage_control),
    get_capabilities,
    ensure_descriptor_ready,
};

rodaq_status_code query_interface(
    void* instance, rodaq_string_view interface_id, rodaq_interface* out_interface,
    rodaq_status*) noexcept {
    if (out_interface == nullptr) {
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    out_interface->context = nullptr;
    out_interface->vtable = nullptr;

    const char expected_id[] = RODAQ_STORAGE_CONTROL_INTERFACE_ID;
    if (!string_equals(interface_id, expected_id, sizeof(expected_id) - 1)) {
        return RODAQ_STATUS_NOT_FOUND;
    }

    out_interface->context = instance;
    out_interface->vtable = &control;
    return RODAQ_STATUS_OK;
}

const rodaq_plugin_descriptor descriptor = {
    sizeof(rodaq_plugin_descriptor),
    0,
    RODAQ_TEST_VIEW("rodaq.storage.symbol_fixture"),
    RODAQ_TEST_VIEW("Storage Symbol Fixture"),
    RODAQ_TEST_VIEW("1.0.0"),
    RODAQ_TEST_VIEW("M0-B symbol qualification fixture"),
};

const rodaq_storage_plugin plugin = {
    RODAQ_STORAGE_PROTOCOL_VERSION,
    sizeof(rodaq_storage_plugin),
    &descriptor,
    create_instance,
    destroy_instance,
    bind_instance,
    configure_instance,
    open_storage,
    submit_record,
    flush_storage,
    close_storage,
    query_interface,
};

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT const uint32_t rodaq_plugin_abi_version = RODAQ_PLUGIN_ABI_VERSION;

extern "C" RODAQ_PLUGIN_EXPORT const rodaq_storage_plugin* rodaq_get_storage_plugin(void) noexcept {
    return &plugin;
}
