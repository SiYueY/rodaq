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
rodaq_status_code start(void*, rodaq_status*) { return 0; }
rodaq_status_code stop(void*, rodaq_status*) { return 0; }
rodaq_status_code prepare(
    void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, rodaq_status*) {
    return 0;
}
rodaq_status_code commit(void*, std::uint64_t, rodaq_status*) { return 0; }
rodaq_status_code abort(void*, std::uint64_t, std::uint32_t, rodaq_status*) { return 0; }
rodaq_status_code quiesce_stream(void*, std::uint64_t, rodaq_status*) { return 0; }
rodaq_status_code quiesce_plugin(void*, rodaq_status*) { return 0; }

const rodaq_acquisition_control control = {
    1,
    sizeof(rodaq_acquisition_control),
    &prepare,
    &commit,
    &abort,
    &quiesce_stream,
    &quiesce_plugin,
};

rodaq_status_code query(void* instance, rodaq_string_view id, rodaq_interface* out, rodaq_status*) {
    *out = {};
    const char expected[] = "rodaq.acquisition.control";
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
    {"rodaq.acquisition.current_abi", sizeof("rodaq.acquisition.current_abi") - 1},
    {"Current Acquisition", sizeof("Current Acquisition") - 1},
    {"0.1.0", 5},
    {"current ABI fixture", sizeof("current ABI fixture") - 1},
};

const rodaq_acquisition_plugin plugin = {
    1,           sizeof(rodaq_acquisition_plugin),
    &descriptor, &create,
    &destroy,    &bind,
    &configure,  &start,
    &stop,       &query,
};

}  // namespace

extern "C" __attribute__((visibility("default"))) const std::uint32_t rodaq_plugin_abi_version = 1;
extern "C" __attribute__((visibility("default"))) const rodaq_acquisition_plugin*
rodaq_get_acquisition_plugin(void) noexcept {
    return &plugin;
}
