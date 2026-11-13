#include <rodaq/plugin/c/abi.h>

#include <cstdint>

namespace {

const rodaq_string_t kPluginId{
    "com.rodaq.test.malformed-create",
    static_cast<std::uint32_t>(sizeof("com.rodaq.test.malformed-create") - 1u)};
const rodaq_string_t kPluginName{
    "Malformed Create Test", static_cast<std::uint32_t>(sizeof("Malformed Create Test") - 1u)};
const rodaq_string_t kPluginVersion{"1.0.0", 5u};
const rodaq_plugin_descriptor_t kDescriptor{
    rodaq_struct_header_t{sizeof(rodaq_plugin_descriptor_t)}, kPluginId, kPluginName,
    kPluginVersion};

rodaq_status_code_t malformed_create(rodaq_plugin_instance_t* instance, rodaq_error_t*) {
    *instance = rodaq_plugin_instance_t{};
    return RODAQ_STATUS_OK;
}

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void) {
    return RODAQ_PLUGIN_ABI_VERSION(1u, 0u);
}

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void) {
    static const rodaq_plugin_entry_t entry{
        rodaq_struct_header_t{sizeof(rodaq_plugin_entry_t)},
        []() -> const rodaq_plugin_descriptor_t* { return &kDescriptor; }, &malformed_create};
    return &entry;
}
