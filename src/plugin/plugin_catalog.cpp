#include "plugin/plugin_catalog.hpp"

#include "plugin/plugin_status.hpp"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace rodaq::plugin {

Result<PluginLibraryLease> PluginCatalog::register_library(PluginLibrary library) {
    if (!library.valid()) {
        return invalid_argument_status("Cannot register an invalid PluginLibrary");
    }
    if (library.lease().quarantined()) {
        return rodaq::Status(
            StatusCode::Unavailable, "plugin", 0, "Cannot register a quarantined Plugin library");
    }

    try {
        const std::string canonical_path = library.canonical_path().string();
        const auto path_entry = plugin_id_by_path_.find(canonical_path);
        if (path_entry != plugin_id_by_path_.end()) {
            return find(path_entry->second);
        }

        const std::string plugin_id = library.metadata().id;
        const auto id_entry = libraries_.find(plugin_id);
        if (id_entry != libraries_.end()) {
            return rodaq::Status(
                StatusCode::Conflict, "plugin", 0,
                "Plugin ID is already registered by a different DSO: " + plugin_id);
        }

        const auto library_entry = libraries_.emplace(plugin_id, std::move(library));
        try {
            plugin_id_by_path_.emplace(canonical_path, plugin_id);
        } catch (...) {
            libraries_.erase(library_entry.first);
            throw;
        }
        return library_entry.first->second.lease();
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while registering Plugin library");
    } catch (const std::exception& error) {
        return internal_status(std::string("Unexpected Plugin catalog failure: ") + error.what());
    }
}

Result<PluginLibraryLease> PluginCatalog::find(std::string_view plugin_id) const {
    try {
        const auto entry = libraries_.find(std::string(plugin_id));
        if (entry == libraries_.end()) {
            return rodaq::Status(
                StatusCode::NotFound, "plugin", 0,
                "Plugin ID is not loaded: " + std::string(plugin_id));
        }
        PluginLibraryLease lease = entry->second.lease();
        if (lease.quarantined()) {
            return rodaq::Status(
                StatusCode::Unavailable, "plugin", 0,
                "Plugin ID is quarantined: " + std::string(plugin_id));
        }
        return lease;
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while querying Plugin catalog");
    } catch (const std::exception& error) {
        return internal_status(
            std::string("Unexpected Plugin catalog query failure: ") + error.what());
    }
}

Result<PluginLibraryLease> PluginCatalog::find_by_path(
    const std::filesystem::path& canonical_path) const {
    try {
        const auto entry = plugin_id_by_path_.find(canonical_path.string());
        if (entry == plugin_id_by_path_.end()) {
            return rodaq::Status(
                StatusCode::NotFound, "plugin", 0,
                "Plugin path is not loaded: " + canonical_path.string());
        }
        return find(entry->second);
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while querying Plugin catalog path");
    } catch (const std::exception& error) {
        return internal_status(
            std::string("Unexpected Plugin catalog path failure: ") + error.what());
    }
}

Result<void> PluginCatalog::remove(std::string_view plugin_id) {
    try {
        const auto entry = libraries_.find(std::string(plugin_id));
        if (entry == libraries_.end()) {
            return rodaq::Status(
                StatusCode::NotFound, "plugin", 0,
                "Plugin ID is not loaded: " + std::string(plugin_id));
        }

        plugin_id_by_path_.erase(entry->second.canonical_path().string());
        libraries_.erase(entry);
        return {};
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "plugin", 0,
            "Memory exhausted while removing Plugin library");
    } catch (const std::exception& error) {
        return internal_status(
            std::string("Unexpected Plugin catalog removal failure: ") + error.what());
    }
}

}  // namespace rodaq::plugin
