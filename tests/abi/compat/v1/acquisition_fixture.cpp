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
rodaq_status start(void*, rodaq_error*) { return 0; }
rodaq_status stop(void*, rodaq_error*) { return 0; }
rodaq_status prepare(
    void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, rodaq_error*) {
    return 0;
}
rodaq_status commit(void*, std::uint64_t, rodaq_error*) { return 0; }
rodaq_status abort(void*, std::uint64_t, std::uint32_t, rodaq_error*) { return 0; }
rodaq_status quiesce_stream(void*, std::uint64_t, rodaq_error*) { return 0; }
rodaq_status quiesce_plugin(void*, rodaq_error*) { return 0; }

const rodaq_acquisition_control_v1 control = {
    1,
    sizeof(rodaq_acquisition_control_v1),
    &prepare,
    &commit,
    &abort,
    &quiesce_stream,
    &quiesce_plugin,
};

rodaq_status query(void* instance, rodaq_string_view id, rodaq_interface* out, rodaq_error*) {
    *out = {};
    const char expected[] = "rodaq.acquisition.control.v1";
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
    {"rodaq.acquisition.compat_v1", 27},
    {"Compat Acquisition", 18},
    {"0.1.0", 5},
    {"frozen V1 fixture", 17},
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
