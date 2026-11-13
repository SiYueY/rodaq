#include <rodaq/plugin/c/abi.h>

#include <cstdint>

namespace {

const rodaq_string_t kPluginId{
    "com.rodaq.test.partial-create-failure",
    static_cast<std::uint32_t>(sizeof("com.rodaq.test.partial-create-failure") - 1u)};
const rodaq_string_t kPluginName{
    "Partial Create Failure Test",
    static_cast<std::uint32_t>(sizeof("Partial Create Failure Test") - 1u)};
const rodaq_string_t kPluginVersion{"1.0.0", 5u};
const rodaq_plugin_descriptor_t kDescriptor{
    rodaq_struct_header_t{sizeof(rodaq_plugin_descriptor_t)}, kPluginId, kPluginName,
    kPluginVersion};

rodaq_status_code_t partial_failure_create(rodaq_plugin_instance_t* instance, rodaq_error_t*) {
    instance->context = reinterpret_cast<rodaq_plugin_context_t*>(static_cast<std::uintptr_t>(1u));
    instance->vtable = nullptr;
    return RODAQ_STATUS_RESOURCE_EXHAUSTED;
}

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void) {
    return RODAQ_PLUGIN_ABI_VERSION(1u, 0u);
}

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void) {
    static const rodaq_plugin_entry_t entry{
        rodaq_struct_header_t{sizeof(rodaq_plugin_entry_t)},
        []() -> const rodaq_plugin_descriptor_t* { return &kDescriptor; }, &partial_failure_create};
    return &entry;
}
