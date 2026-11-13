#pragma once

#include "artifact.hpp"
#include "manifest.hpp"

#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rodaq::plugin {

struct CatalogEntry final {
    Manifest manifest;
    std::filesystem::path package_root;
    std::filesystem::path artifact_path;
    ArtifactDigest artifact;
};

struct CatalogDiagnostic final {
    std::filesystem::path package_root;
    StatusCode code = StatusCode::Internal;
    std::string message;
};

struct CatalogGeneration final {
    std::vector<CatalogEntry> entries;
    std::vector<CatalogDiagnostic> diagnostics;
};

struct CatalogLimits final {
    std::size_t max_roots = 16u;
    std::size_t max_packages = 4096u;
    std::size_t max_package_entries = 16384u;
    std::size_t max_manifest_size = 1024u * 1024u;
    std::size_t max_package_files = 1024u;
    std::uint64_t max_artifact_size = 512u * 1024u * 1024u;
    std::uint64_t max_scan_bytes = 64u * 1024u * 1024u;
    std::size_t max_path_length = 4096u;
    std::size_t max_diagnostics = 256u;
    std::size_t max_diagnostic_bytes = 64u * 1024u;
};

[[nodiscard]] Result<CatalogGeneration> discover_catalog(
    const std::vector<std::filesystem::path>& roots, CatalogLimits limits = {});

}  // namespace rodaq::plugin
