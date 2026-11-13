#include "loader.hpp"

#include "abi_support.hpp"
#include "semver.hpp"

#include <rodaq/plugin/c/abi.h>
#include <rodaq/status.hpp>

#include <dlfcn.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace rodaq::plugin {
namespace {

class TransientLibrary final {
public:
    explicit TransientLibrary(void* handle = nullptr) noexcept : handle_(handle) {}
    TransientLibrary(const TransientLibrary&) = delete;
    TransientLibrary& operator=(const TransientLibrary&) = delete;
    ~TransientLibrary() {
        if (handle_ != nullptr) {
            ::dlclose(handle_);
        }
    }

    [[nodiscard]] void* get() const noexcept { return handle_; }
    void release() noexcept { handle_ = nullptr; }

private:
    void* handle_;
};

bool string_valid(rodaq_string_t value) noexcept {
    return value.size <= 1024u && valid_string(value);
}

std::string_view string_view(rodaq_string_t value) noexcept {
    return string_valid(value) ? string_view_from_abi(value) : std::string_view{};
}

Result<void> validate_descriptor(
    const rodaq_plugin_descriptor_t* descriptor, const Manifest& manifest) {
    if (descriptor == nullptr ||
        descriptor->header.struct_size < sizeof(rodaq_plugin_descriptor_t)) {
        return Status{StatusCode::InvalidArgument, "plugin descriptor is incomplete"};
    }
    if (!string_valid(descriptor->plugin_id) || !string_valid(descriptor->plugin_name) ||
        !string_valid(descriptor->plugin_version)) {
        return Status{StatusCode::InvalidArgument, "plugin descriptor string is invalid"};
    }
    const auto version = parse_semver(string_view(descriptor->plugin_version));
    if (!version.ok()) {
        return Status{StatusCode::InvalidArgument, "plugin descriptor version is invalid"};
    }
    if (string_view(descriptor->plugin_id) != manifest.plugin_id ||
        string_view(descriptor->plugin_name) != manifest.plugin_name ||
        string_view(descriptor->plugin_version) != manifest.plugin_version) {
        return Status{StatusCode::Conflict, "plugin descriptor does not match manifest"};
    }
    return {};
}

Result<void> validate_provenance(
    const ArtifactDigest& artifact, const std::filesystem::path& artifact_path,
    const void* symbol) {
    Dl_info info{};
    if (::dladdr(symbol, &info) == 0 || info.dli_fname == nullptr) {
        return Status{StatusCode::PermissionDenied, "plugin symbol provenance is unavailable"};
    }
    std::error_code error;
    const auto loaded_path = std::filesystem::weakly_canonical(info.dli_fname, error);
    if (error || loaded_path != std::filesystem::weakly_canonical(artifact_path)) {
        return Status{StatusCode::PermissionDenied, "plugin symbol came from another object"};
    }
    auto loaded_identity = file_identity(loaded_path);
    if (!loaded_identity.ok() || !same_file_identity(artifact.identity, loaded_identity.value())) {
        return Status{StatusCode::PermissionDenied, "loaded plugin object identity changed"};
    }
    return {};
}

}  // namespace

Result<std::shared_ptr<PluginLibraryState>> load_plugin_library(const CatalogEntry& entry) {
    auto verified =
        verify_artifact(entry.artifact_path, entry.manifest.artifact_sha256, 512u * 1024u * 1024u);
    if (!verified.ok()) {
        return verified.status();
    }
    if (!same_file_identity(verified.value().identity, entry.artifact.identity)) {
        return Status{StatusCode::PermissionDenied, "plugin artifact changed after discovery"};
    }

    auto fallback_artifact = std::move(verified).value();
    TransientLibrary library{::dlopen(entry.artifact_path.c_str(), RTLD_NOW | RTLD_LOCAL)};
    if (library.get() == nullptr) {
        return Status{StatusCode::Unavailable, "plugin dynamic library could not be loaded"};
    }

    const auto abi_version = reinterpret_cast<rodaq_plugin_abi_version_fn_t>(
        ::dlsym(library.get(), "rodaq_plugin_abi_version"));
    const auto get_entry = reinterpret_cast<rodaq_plugin_get_entry_fn_t>(
        ::dlsym(library.get(), "rodaq_plugin_get_entry"));
    if (abi_version == nullptr || get_entry == nullptr) {
        return Status{StatusCode::NotFound, "plugin bootstrap symbol is missing"};
    }

    const auto abi_provenance = validate_provenance(
        fallback_artifact, entry.artifact_path, reinterpret_cast<const void*>(abi_version));
    if (!abi_provenance.ok()) {
        return abi_provenance.status();
    }
    const auto entry_provenance = validate_provenance(
        fallback_artifact, entry.artifact_path, reinterpret_cast<const void*>(get_entry));
    if (!entry_provenance.ok()) {
        return entry_provenance.status();
    }

    const auto version = abi_version();
    if (RODAQ_PLUGIN_ABI_MAJOR(version) != kSupportedPluginAbiMajor ||
        RODAQ_PLUGIN_ABI_MINOR(version) > kSupportedPluginAbiMinor) {
        return Status{StatusCode::Unavailable, "plugin ABI is incompatible"};
    }
    if (RODAQ_PLUGIN_ABI_MAJOR(version) != entry.manifest.abi_major ||
        RODAQ_PLUGIN_ABI_MINOR(version) != entry.manifest.abi_minor) {
        return Status{StatusCode::Conflict, "plugin ABI does not match manifest"};
    }

    const auto* plugin_entry = get_entry();
    if (plugin_entry == nullptr ||
        plugin_entry->header.struct_size < sizeof(rodaq_plugin_entry_t) ||
        plugin_entry->descriptor == nullptr || plugin_entry->create == nullptr) {
        return Status{StatusCode::InvalidArgument, "plugin entry is incomplete"};
    }
    const auto* descriptor = plugin_entry->descriptor();
    const auto descriptor_status = validate_descriptor(descriptor, entry.manifest);
    if (!descriptor_status.ok()) {
        return descriptor_status.status();
    }

    auto state = std::make_shared<PluginLibraryState>(
        library.get(), entry.manifest, std::move(fallback_artifact), plugin_entry);
    library.release();
    return state;
}

}  // namespace rodaq::plugin
