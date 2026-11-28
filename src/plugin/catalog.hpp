#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/plugin/types.hpp>
#include <rodaq/result.hpp>

namespace rodaq::plugin {

// Private Loader defense limits; no runtime discovery generation or public knobs.
inline constexpr std::size_t kMaxPluginRoots = 16;
inline constexpr std::size_t kMaxPackageEntries = 256;
inline constexpr std::size_t kMaxScanFiles = 8192;
inline constexpr std::size_t kMaxManifestSize = 64 * 1024;
inline constexpr std::size_t kMaxPathLength = 4096;
inline constexpr std::size_t kMaxCatalogDiagnostics = 64;
inline constexpr std::size_t kMaxCatalogDiagnosticBytes = 256;

struct CatalogEntry {
    PluginInfo info;
    std::filesystem::path root;
    std::filesystem::path library_path;
    std::uint16_t abi_major{0};
    std::uint16_t abi_minor{0};
};

Result<void> parse_semver(std::string_view value);
Result<std::vector<CatalogEntry>> discover_plugins(const std::vector<std::filesystem::path>& roots);
Result<std::size_t> select_plugin(
    const std::vector<CatalogEntry>& catalog, std::string_view id,
    std::optional<std::string_view> expected_version);

}  // namespace rodaq::plugin
