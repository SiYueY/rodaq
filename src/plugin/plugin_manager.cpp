#include "plugin/plugin_manager.hpp"

#include "plugin/plugin_status.hpp"
#include "plugin/shared_library.hpp"

#include <exception>
#include <new>
#include <utility>

namespace rodaq::plugin {

Result<PluginLibraryLease> PluginManager::load_library(const std::filesystem::path& path) {
    Result<std::filesystem::path> canonical_result = SharedLibrary::canonicalize_path(path);
    if (!canonical_result.ok()) {
        return std::move(canonical_result).status();
    }
    const std::filesystem::path canonical = std::move(canonical_result.value());

    Result<PluginLibraryLease> existing = catalog_.find_by_path(canonical);
    if (existing.ok()) {
        return existing;
    }
    if (existing.status().code() != StatusCode::NotFound) {
        return std::move(existing).status();
    }

    Result<PluginLibrary> library_result = PluginLibrary::open(canonical);
    if (!library_result.ok()) {
        return std::move(library_result).status();
    }
    return catalog_.register_library(std::move(library_result.value()));
}

Result<PluginLibraryLease> PluginManager::find(std::string_view plugin_id) const {
    return catalog_.find(plugin_id);
}

Result<AcquisitionPluginInstance> PluginManager::create_acquisition_plugin_instance(
    PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Acquisition) {
        return invalid_argument_status(
            "Requested Acquisition instance from a non-Acquisition Plugin");
    }
    return AcquisitionPluginInstance::create(std::move(library_lease));
}

Result<StoragePluginInstance> PluginManager::create_storage_plugin_instance(
    PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Storage) {
        return invalid_argument_status("Requested Storage instance from a non-Storage Plugin");
    }
    return StoragePluginInstance::create(std::move(library_lease));
}

Result<void> PluginManager::unload(std::string_view plugin_id) {
    return catalog_.remove(plugin_id);
}

void PluginManager::quarantine(PluginInstanceLease instance_lease) noexcept {
    instance_lease.quarantine();
}

}  // namespace rodaq::plugin
