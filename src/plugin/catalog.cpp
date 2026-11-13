#include "catalog.hpp"

#include "abi_support.hpp"

#include <rodaq/status.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace rodaq::plugin {
namespace {

void add_diagnostic(
    CatalogGeneration& generation, const std::filesystem::path& package_root, const Status& status,
    CatalogLimits limits) {
    if (generation.diagnostics.size() >= limits.max_diagnostics) {
        return;
    }
    std::size_t used_bytes = package_root.generic_string().size();
    for (const auto& diagnostic : generation.diagnostics) {
        used_bytes += diagnostic.package_root.generic_string().size() + diagnostic.message.size();
    }
    if (used_bytes >= limits.max_diagnostic_bytes) {
        return;
    }
    const auto available = limits.max_diagnostic_bytes - used_bytes;
    auto message_size = std::min(available, status.message().size());
    while (message_size > 0u && !valid_utf8_text(status.message().substr(0u, message_size))) {
        --message_size;
    }
    generation.diagnostics.push_back(CatalogDiagnostic{
        package_root, status.code(), std::string{status.message().substr(0u, message_size)}});
}

bool contained(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto root_text = root.generic_string();
    const auto candidate_text = candidate.generic_string();
    return candidate_text == root_text ||
           (candidate_text.size() > root_text.size() &&
            candidate_text.compare(0u, root_text.size(), root_text) == 0 &&
            candidate_text[root_text.size()] == '/');
}

Result<CatalogEntry> inspect_package(
    const std::filesystem::path& package_root, CatalogLimits limits) {
    std::error_code error;
    auto canonical_root = std::filesystem::weakly_canonical(package_root, error);
    if (error) {
        return Status{StatusCode::Unavailable, "plugin package root cannot be canonicalized"};
    }

    const auto manifest_path = canonical_root / "manifest.json";
    auto manifest = load_manifest(manifest_path, ManifestLimits{limits.max_manifest_size, 16u});
    if (!manifest.ok()) {
        return manifest.status();
    }

    auto artifact_path =
        std::filesystem::weakly_canonical(canonical_root / manifest.value().artifact_path, error);
    if (error || !contained(canonical_root, artifact_path)) {
        return Status{StatusCode::InvalidArgument, "artifact path escapes plugin package"};
    }

    std::size_t package_files = 0u;
    std::uint64_t scan_bytes = manifest.value().artifact_path.size();
    std::filesystem::recursive_directory_iterator iterator{canonical_root, error};
    if (error) {
        return Status{StatusCode::Unavailable, "plugin package cannot be enumerated"};
    }
    for (const auto& item : iterator) {
        if (item.path().generic_string().size() > limits.max_path_length) {
            return Status{StatusCode::ResourceExhausted, "plugin package path is too long"};
        }
        ++package_files;
        scan_bytes += item.path().generic_string().size();
        if (package_files > limits.max_package_files || scan_bytes > limits.max_scan_bytes) {
            return Status{StatusCode::ResourceExhausted, "plugin package scan exceeds limit"};
        }
    }

    auto artifact =
        verify_artifact(artifact_path, manifest.value().artifact_sha256, limits.max_artifact_size);
    if (!artifact.ok()) {
        return artifact.status();
    }

    return CatalogEntry{
        std::move(manifest).value(), std::move(canonical_root), std::move(artifact_path),
        std::move(artifact).value()};
}

}  // namespace

Result<CatalogGeneration> discover_catalog(
    const std::vector<std::filesystem::path>& roots, CatalogLimits limits) {
    if (roots.size() > limits.max_roots) {
        return Status{StatusCode::ResourceExhausted, "plugin root count exceeds limit"};
    }

    CatalogGeneration generation;
    std::size_t package_count = 0u;
    std::size_t package_entries = 0u;
    for (const auto& root : roots) {
        std::error_code error;
        const auto canonical_root = std::filesystem::weakly_canonical(root, error);
        if (error || !std::filesystem::is_directory(canonical_root, error)) {
            add_diagnostic(
                generation, root, Status{StatusCode::Unavailable, "plugin root is not a directory"},
                limits);
            continue;
        }

        std::filesystem::directory_iterator iterator{canonical_root, error};
        if (error) {
            add_diagnostic(
                generation, canonical_root,
                Status{StatusCode::Unavailable, "plugin root cannot be enumerated"}, limits);
            continue;
        }
        for (const auto& item : iterator) {
            if (++package_entries > limits.max_package_entries) {
                return Status{
                    StatusCode::ResourceExhausted, "plugin package entry count exceeds limit"};
            }
            if (package_count >= limits.max_packages) {
                add_diagnostic(
                    generation, canonical_root,
                    Status{StatusCode::ResourceExhausted, "plugin package count exceeds limit"},
                    limits);
                break;
            }
            if (item.path().generic_string().size() > limits.max_path_length) {
                add_diagnostic(
                    generation, item.path(),
                    Status{StatusCode::ResourceExhausted, "plugin package path is too long"},
                    limits);
                continue;
            }
            if (!item.is_directory(error)) {
                continue;
            }
            ++package_count;
            auto entry = inspect_package(item.path(), limits);
            if (!entry.ok()) {
                add_diagnostic(generation, item.path(), entry.status(), limits);
                continue;
            }
            generation.entries.push_back(std::move(entry).value());
        }
    }

    std::sort(
        generation.entries.begin(), generation.entries.end(),
        [](const CatalogEntry& left, const CatalogEntry& right) {
            if (left.manifest.plugin_id != right.manifest.plugin_id) {
                return left.manifest.plugin_id < right.manifest.plugin_id;
            }
            return left.manifest.plugin_version < right.manifest.plugin_version;
        });
    return generation;
}

}  // namespace rodaq::plugin
