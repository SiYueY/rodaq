#include "frozen_abi.h"

#include <cstdint>
#include <string_view>

namespace {

struct Instance {
    int value;
};

bool equals(rodaq_string_view value, const char* text, std::uint64_t size) {
    return value.size == size && value.data != nullptr &&
           std::string_view(value.data, size) == std::string_view(text, size);
}

rodaq_status create(void** out, rodaq_error*) {
    static Instance instance{};
    *out = &instance;
    return RODAQ_STATUS_OK;
}
void destroy(void*) {}
rodaq_status bind(void*, const rodaq_service_registry*, rodaq_error*) { return 0; }
rodaq_status configure(void*, rodaq_config_view, rodaq_error*) { return 0; }
rodaq_status open(void*, const void*, rodaq_error*) { return 0; }
rodaq_status submit(void*, const void*, void*, rodaq_error*) { return 0; }
rodaq_status flush(void*, rodaq_error*) { return 0; }
rodaq_status close(void*, const void*, rodaq_error*) { return 0; }

typedef struct {
    uint32_t protocol_version;
    uint32_t struct_size;
    rodaq_status (*get_capabilities)(void*, void*, rodaq_error*);
    rodaq_status (*ensure_descriptor_ready)(void*, uint64_t, void*, rodaq_error*);
} storage_control;

rodaq_status get_capabilities(void*, void*, rodaq_error*) { return 0; }
rodaq_status ensure_ready(void*, uint64_t, void*, rodaq_error*) { return 0; }
const storage_control control = {1, sizeof(storage_control), &get_capabilities, &ensure_ready};

rodaq_status query(void* instance, rodaq_string_view id, rodaq_interface* out, rodaq_error*) {
    *out = {};
    const char expected[] = "rodaq.storage.control.v1";
    if (!equals(id, expected, sizeof(expected) - 1)) {
        return RODAQ_STATUS_NOT_FOUND;
    }
    out->context = instance;
    out->vtable = &control;
    return RODAQ_STATUS_OK;
}

const rodaq_plugin_descriptor descriptor = {
    sizeof(rodaq_plugin_descriptor),
    0,
    {"rodaq.storage.compat_v1", 23},
    {"Compat Storage", 14},
    {"0.1.0", 5},
    {"frozen V1 fixture", 17},
};

typedef struct {
    uint32_t protocol_version;
    uint32_t struct_size;
    const rodaq_plugin_descriptor* descriptor;
    rodaq_status (*create)(void**, rodaq_error*);
    void (*destroy)(void*);
    rodaq_status (*bind)(void*, const rodaq_service_registry*, rodaq_error*);
    rodaq_status (*configure)(void*, rodaq_config_view, rodaq_error*);
    rodaq_status (*open)(void*, const void*, rodaq_error*);
    rodaq_status (*submit)(void*, const void*, void*, rodaq_error*);
    rodaq_status (*flush)(void*, rodaq_error*);
    rodaq_status (*close)(void*, const void*, rodaq_error*);
    rodaq_status (*query_interface)(void*, rodaq_string_view, rodaq_interface*, rodaq_error*);
} storage_plugin_prefix;

const storage_plugin_prefix plugin = {
    1,           sizeof(storage_plugin_prefix),
    &descriptor, &create,
    &destroy,    &bind,
    &configure,  &open,
    &submit,     &flush,
    &close,      &query,
};

}  // namespace

extern "C" __attribute__((visibility("default"))) const std::uint32_t rodaq_plugin_abi_version = 1;
extern "C" __attribute__((visibility("default"))) const storage_plugin_prefix*
rodaq_get_storage_plugin(void) noexcept {
    return &plugin;
}
