#include <rodaq/plugin/c/abi.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace {

const rodaq_string_t kPluginId{
    "com.rodaq.test.invalid-success",
    static_cast<std::uint32_t>(sizeof("com.rodaq.test.invalid-success") - 1u)};
const rodaq_string_t kPluginName{
    "Invalid Success Test", static_cast<std::uint32_t>(sizeof("Invalid Success Test") - 1u)};
const rodaq_string_t kPluginVersion{"1.0.0", 5u};
const rodaq_plugin_descriptor_t kDescriptor{
    rodaq_struct_header_t{sizeof(rodaq_plugin_descriptor_t)}, kPluginId, kPluginName,
    kPluginVersion};

void write_unexpected_error(rodaq_error_t* error) noexcept {
    if (error == nullptr || error->message.data == nullptr || error->message.capacity == 0u) {
        return;
    }
    constexpr std::string_view kMessage = "unexpected success diagnostic";
    const auto size = std::min<std::size_t>(kMessage.size(), error->message.capacity);
    std::copy_n(kMessage.data(), size, error->message.data);
    error->message.size = static_cast<std::uint32_t>(size);
}

rodaq_plugin_instance_t kNeverReturnedInstance{};

rodaq_status_code_t invalid_success_create(
    rodaq_plugin_instance_t* instance, rodaq_error_t* error) {
    *instance = kNeverReturnedInstance;
    write_unexpected_error(error);
    return RODAQ_STATUS_OK;
}

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void) {
    return RODAQ_PLUGIN_ABI_VERSION(1u, 0u);
}

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void) {
    static const rodaq_plugin_entry_t entry{
        rodaq_struct_header_t{sizeof(rodaq_plugin_entry_t)},
        []() -> const rodaq_plugin_descriptor_t* { return &kDescriptor; }, &invalid_success_create};
    return &entry;
}
