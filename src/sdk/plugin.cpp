#include <rodaq/plugin/plugin.hpp>
#include <rodaq/plugin/acquisition_plugin.hpp>
#include <rodaq/plugin/storage_plugin.hpp>

namespace rodaq::plugin {

rodaq_status_code invalid_input(const char* message, rodaq_status* out_status) noexcept {
    assign_abi_status(RODAQ_STATUS_INVALID_ARGUMENT, "sdk", 0, message, out_status);
    return RODAQ_STATUS_INVALID_ARGUMENT;
}

}  // namespace rodaq::plugin

namespace rodaq {
Result<InterfaceView> AcquisitionPlugin::query_interface(std::string_view interface_id) {
    static_cast<void>(interface_id);
    return rodaq::Status(
        StatusCode::NotFound, "sdk", 0, "Optional Plugin interface is unavailable");
}

Result<InterfaceView> StoragePlugin::query_interface(std::string_view interface_id) {
    static_cast<void>(interface_id);
    return rodaq::Status(
        StatusCode::NotFound, "sdk", 0, "Optional Plugin interface is unavailable");
}
}  // namespace rodaq
