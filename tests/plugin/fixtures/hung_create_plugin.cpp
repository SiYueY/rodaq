#include <rodaq/plugin/c/abi.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace {

const rodaq_string_t kPluginId{
    "com.rodaq.test.hung-create",
    static_cast<std::uint32_t>(sizeof("com.rodaq.test.hung-create") - 1u)};
const rodaq_string_t kPluginName{
    "Hung Create Test", static_cast<std::uint32_t>(sizeof("Hung Create Test") - 1u)};
const rodaq_string_t kPluginVersion{"1.0.0", 5u};
const rodaq_plugin_descriptor_t kDescriptor{
    rodaq_struct_header_t{sizeof(rodaq_plugin_descriptor_t)}, kPluginId, kPluginName,
    kPluginVersion};

rodaq_status_code_t unused_configure(
    rodaq_plugin_context_t*, const rodaq_service_registry_reference_t*, const rodaq_config_t*,
    rodaq_error_t*) {
    return RODAQ_STATUS_INVALID_STATE;
}

rodaq_status_code_t unused_query(
    rodaq_plugin_context_t*, const rodaq_interface_query_t*, rodaq_interface_reference_t*,
    rodaq_error_t*) {
    return RODAQ_STATUS_NOT_FOUND;
}

rodaq_status_code_t unused_shutdown(rodaq_plugin_context_t*, rodaq_deadline_t, rodaq_error_t*) {
    return RODAQ_STATUS_OK;
}

void unused_destroy(rodaq_plugin_context_t*) {}

const rodaq_plugin_instance_vtable_t kVtable{
    rodaq_struct_header_t{sizeof(rodaq_plugin_instance_vtable_t)}, &unused_configure, &unused_query,
    &unused_shutdown, &unused_destroy};

rodaq_status_code_t slow_failure_create(rodaq_plugin_instance_t* instance, rodaq_error_t*) {
    *instance = rodaq_plugin_instance_t{};
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    return RODAQ_STATUS_RESOURCE_EXHAUSTED;
}

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void) {
    return RODAQ_PLUGIN_ABI_VERSION(1u, 0u);
}

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void) {
    static const rodaq_plugin_entry_t entry{
        rodaq_struct_header_t{sizeof(rodaq_plugin_entry_t)},
        []() -> const rodaq_plugin_descriptor_t* { return &kDescriptor; }, &slow_failure_create};
    return &entry;
}
