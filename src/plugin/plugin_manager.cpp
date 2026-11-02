#include "plugin/plugin_manager.hpp"

#include "plugin/plugin_error.hpp"
#include "plugin/shared_library.hpp"

#include <exception>
#include <new>
#include <utility>

namespace rodaq::plugin {

Result<PluginLibraryLease> PluginManager::load_library(const std::filesystem::path& path) {
    Result<std::filesystem::path> canonical_result = SharedLibrary::canonicalize_path(path);
    if (!canonical_result.has_value()) {
        return nonstd::make_unexpected(std::move(canonical_result.error()));
    }
    const std::filesystem::path canonical = std::move(canonical_result.value());

    Result<PluginLibraryLease> existing = catalog_.find_by_path(canonical);
    if (existing.has_value()) {
        return existing;
    }
    if (existing.error().code() != ErrorCode::NotFound) {
        return nonstd::make_unexpected(std::move(existing.error()));
    }

    Result<PluginLibrary> library_result = PluginLibrary::open(canonical);
    if (!library_result.has_value()) {
        return nonstd::make_unexpected(std::move(library_result.error()));
    }
    return catalog_.register_library(std::move(library_result.value()));
}

Result<PluginLibraryLease> PluginManager::find(std::string_view plugin_id) const {
    return catalog_.find(plugin_id);
}

Result<AcquisitionPluginInstance> PluginManager::create_acquisition_plugin_instance(
    PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Acquisition) {
        return nonstd::make_unexpected(
            invalid_argument_error("Requested Acquisition instance from a non-Acquisition Plugin"));
    }
    return AcquisitionPluginInstance::create(std::move(library_lease));
}

Result<StoragePluginInstance> PluginManager::create_storage_plugin_instance(
    PluginLibraryLease library_lease) {
    if (!library_lease.valid() || library_lease.family() != PluginFamily::Storage) {
        return nonstd::make_unexpected(
            invalid_argument_error("Requested Storage instance from a non-Storage Plugin"));
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
