#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <rodaq/plugin/registry.hpp>

namespace rodaq::plugin {

struct RegistryState {
    RegistryState();

    std::mutex mutex;
    std::atomic<bool> frozen{false};
    std::vector<authoring::InterfaceEntry> entries;
    std::vector<std::shared_ptr<void>> owners;
};

rodaq_status_code_t registry_query(
    const RegistryState& registry, const rodaq_interface_query_t* query,
    rodaq_interface_reference_t* reference, rodaq_error_t* error) noexcept;

}  // namespace rodaq::plugin
