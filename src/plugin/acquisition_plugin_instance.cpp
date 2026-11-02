#include "plugin/acquisition_plugin_instance.hpp"

#include "plugin/plugin_error.hpp"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace rodaq::plugin {

Result<AcquisitionPluginInstance> AcquisitionPluginInstance::create(
    PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Acquisition) {
        return nonstd::make_unexpected(invalid_argument_error(
            "Acquisition instance requires a valid Acquisition PluginLibraryLease"));
    }
    if (library_lease.quarantined()) {
        return nonstd::make_unexpected(Error(
            ErrorCode::Unavailable, "plugin", 0,
            "Cannot create an Acquisition instance from a quarantined Plugin"));
    }

    const rodaq_acquisition_plugin* protocol = library_lease.acquisition_protocol();
    if (protocol == nullptr || protocol->create == nullptr || protocol->destroy == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Acquisition family protocol is missing create/destroy"));
    }

    void* raw_instance = nullptr;
    rodaq_error status_error{};
    const rodaq_status status = protocol->create(&raw_instance, &status_error);
    if (status != RODAQ_STATUS_OK) {
        if (raw_instance != nullptr) {
            library_lease.quarantine();
        }
        Result<void> converted = result_from_status(status, &status_error, "plugin");
        return nonstd::make_unexpected(std::move(converted.error()));
    }
    if (raw_instance == nullptr) {
        return nonstd::make_unexpected(
            invalid_argument_error("Acquisition create() returned OK with a null instance"));
    }

    try {
        auto state =
            PluginInstanceStateAccess::create(library_lease, raw_instance, protocol->destroy);
        return AcquisitionPluginInstance(std::move(state));
    } catch (const std::bad_alloc&) {
        protocol->destroy(raw_instance);
        return nonstd::make_unexpected(Error(
            ErrorCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while tracking Acquisition instance lifetime"));
    } catch (const std::exception& error) {
        protocol->destroy(raw_instance);
        return nonstd::make_unexpected(internal_error(
            std::string("Failed to track Acquisition instance lifetime: ") + error.what()));
    }
}

AcquisitionPluginInstance::AcquisitionPluginInstance(
    std::shared_ptr<PluginInstanceState> state) noexcept
: state_(std::move(state)) {}

PluginInstanceLease AcquisitionPluginInstance::lease() const noexcept {
    return PluginInstanceLease(state_);
}

bool AcquisitionPluginInstance::valid() const noexcept { return state_ != nullptr; }

}  // namespace rodaq::plugin
