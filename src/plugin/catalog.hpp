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
inline constexpr std::size_t kMaxPackageFiles = 128;
inline constexpr std::size_t kMaxScanFiles = 8192;
inline constexpr std::size_t kMaxManifestSize = 64 * 1024;
inline constexpr std::uint64_t kMaxArtifactSize = 128 * 1024 * 1024;
inline constexpr std::uint64_t kMaxScanBytes = 512 * 1024 * 1024;
inline constexpr std::size_t kMaxPathLength = 4096;
inline constexpr std::size_t kMaxCatalogDiagnostics = 64;
inline constexpr std::size_t kMaxCatalogDiagnosticBytes = 256;

struct PluginVersion {
    std::string major;
    std::string minor;
    std::string patch;
    std::vector<std::string> prerelease;
};

Result<PluginVersion> parse_version(std::string_view value);
int compare_version(const PluginVersion& left, const PluginVersion& right) noexcept;

struct FileIdentity {
    std::uint64_t device{0};
    std::uint64_t inode{0};
    std::uint64_t size{0};
    std::int64_t modified_seconds{0};
    std::int64_t modified_nanoseconds{0};
    std::int64_t changed_seconds{0};
    std::int64_t changed_nanoseconds{0};
};

bool same_file(const FileIdentity& left, const FileIdentity& right) noexcept;
Result<FileIdentity> file_identity(const std::filesystem::path& path);

struct CatalogEntry {
    PluginInfo info;
    PluginBinaryInfo binary;
    PluginVersion version;
    std::filesystem::path root;
    std::filesystem::path search_root;
    std::filesystem::path artifact;
    FileIdentity identity;
    std::uint16_t abi_major{0};
    std::uint16_t abi_minor{0};
};

Result<std::vector<CatalogEntry>> discover_plugins(const std::vector<std::filesystem::path>& roots);
Result<std::size_t> select_plugin(
    const std::vector<CatalogEntry>& catalog, std::string_view id,
    std::optional<std::string_view> version);
Result<FileIdentity> verify_artifact(const CatalogEntry& entry);

}  // namespace rodaq::plugin
