#include "baseline_abi.h"

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

rodaq_status_code create(void** out, rodaq_status*) {
    static Instance instance{};
    *out = &instance;
    return RODAQ_STATUS_OK;
}
void destroy(void*) {}
rodaq_status_code bind(void*, const rodaq_service_registry*, rodaq_status*) { return 0; }
rodaq_status_code configure(void*, rodaq_config_view, rodaq_status*) { return 0; }
rodaq_status_code open(void*, const void*, rodaq_status*) { return 0; }
rodaq_status_code submit(void*, const void*, void*, rodaq_status*) { return 0; }
rodaq_status_code flush(void*, rodaq_status*) { return 0; }
rodaq_status_code close(void*, const void*, rodaq_status*) { return 0; }

typedef struct {
    uint32_t protocol_version;
    uint32_t struct_size;
    rodaq_status_code (*get_capabilities)(void*, void*, rodaq_status*);
    rodaq_status_code (*ensure_descriptor_ready)(void*, uint64_t, void*, rodaq_status*);
} storage_control;

rodaq_status_code get_capabilities(void*, void*, rodaq_status*) { return 0; }
rodaq_status_code ensure_ready(void*, uint64_t, void*, rodaq_status*) { return 0; }
const storage_control control = {1, sizeof(storage_control), &get_capabilities, &ensure_ready};

rodaq_status_code query(void* instance, rodaq_string_view id, rodaq_interface* out, rodaq_status*) {
    *out = {};
    const char expected[] = "rodaq.storage.control";
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
    {"rodaq.storage.current_abi", sizeof("rodaq.storage.current_abi") - 1},
    {"Current Storage", sizeof("Current Storage") - 1},
    {"0.1.0", 5},
    {"current ABI fixture", sizeof("current ABI fixture") - 1},
};

typedef struct {
    uint32_t protocol_version;
    uint32_t struct_size;
    const rodaq_plugin_descriptor* descriptor;
    rodaq_status_code (*create)(void**, rodaq_status*);
    void (*destroy)(void*);
    rodaq_status_code (*bind)(void*, const rodaq_service_registry*, rodaq_status*);
    rodaq_status_code (*configure)(void*, rodaq_config_view, rodaq_status*);
    rodaq_status_code (*open)(void*, const void*, rodaq_status*);
    rodaq_status_code (*submit)(void*, const void*, void*, rodaq_status*);
    rodaq_status_code (*flush)(void*, rodaq_status*);
    rodaq_status_code (*close)(void*, const void*, rodaq_status*);
    rodaq_status_code (*query_interface)(void*, rodaq_string_view, rodaq_interface*, rodaq_status*);
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
