#include <rodaq/plugin/c/abi.h>

#include <cstdint>

namespace {

const rodaq_string_t kPluginId{
    "com.rodaq.test.unknown-operation-status",
    static_cast<std::uint32_t>(sizeof("com.rodaq.test.unknown-operation-status") - 1u)};
const rodaq_string_t kPluginName{
    "Unknown Operation Status Test",
    static_cast<std::uint32_t>(sizeof("Unknown Operation Status Test") - 1u)};
const rodaq_string_t kPluginVersion{"1.0.0", 5u};
const rodaq_plugin_descriptor_t kDescriptor{
    rodaq_struct_header_t{sizeof(rodaq_plugin_descriptor_t)}, kPluginId, kPluginName,
    kPluginVersion};

struct OperationVtable {
    rodaq_struct_header_t header;
    rodaq_status_code_t (*call)(rodaq_interface_context_t*, rodaq_error_t*);
};

int marker = 0;

rodaq_status_code_t unknown_call(rodaq_interface_context_t*, rodaq_error_t*) { return 99u; }

const OperationVtable kOperationVtable{
    rodaq_struct_header_t{sizeof(OperationVtable)}, &unknown_call};

rodaq_status_code_t configure(
    rodaq_plugin_context_t*, const rodaq_service_registry_reference_t*, const rodaq_config_t*,
    rodaq_error_t*) {
    return RODAQ_STATUS_OK;
}

rodaq_status_code_t query(
    rodaq_plugin_context_t*, const rodaq_interface_query_t*, rodaq_interface_reference_t* output,
    rodaq_error_t*) {
    *output = rodaq_interface_reference_t{
        reinterpret_cast<rodaq_interface_context_t*>(&marker),
        reinterpret_cast<const rodaq_interface_vtable_t*>(&kOperationVtable),
        sizeof(kOperationVtable), rodaq_interface_version_t{1u, 0u}};
    return RODAQ_STATUS_OK;
}

rodaq_status_code_t shutdown(rodaq_plugin_context_t*, rodaq_deadline_t, rodaq_error_t*) {
    return RODAQ_STATUS_OK;
}

void destroy(rodaq_plugin_context_t*) {}

const rodaq_plugin_instance_vtable_t kInstanceVtable{
    rodaq_struct_header_t{sizeof(rodaq_plugin_instance_vtable_t)}, &configure, &query, &shutdown,
    &destroy};

rodaq_status_code_t create(rodaq_plugin_instance_t* instance, rodaq_error_t*) {
    instance->context = reinterpret_cast<rodaq_plugin_context_t*>(&marker);
    instance->vtable = &kInstanceVtable;
    return RODAQ_STATUS_OK;
}

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void) {
    return RODAQ_PLUGIN_ABI_VERSION(1u, 0u);
}

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void) {
    static const rodaq_plugin_entry_t entry{
        rodaq_struct_header_t{sizeof(rodaq_plugin_entry_t)},
        []() -> const rodaq_plugin_descriptor_t* { return &kDescriptor; }, &create};
    return &entry;
}
