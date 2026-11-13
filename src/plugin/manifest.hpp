#pragma once

#include <rodaq/result.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace rodaq::plugin {

struct Manifest final {
    std::uint32_t format_version = 0u;
    std::string plugin_id;
    std::string plugin_name;
    std::string plugin_version;
    std::uint16_t abi_major = 0u;
    std::uint16_t abi_minor = 0u;
    std::string os;
    std::string arch;
    std::string artifact_path;
    std::string artifact_sha256;
};

struct ManifestLimits final {
    std::uint64_t max_manifest_size = 1024u * 1024u;
    std::size_t max_depth = 16u;
};

[[nodiscard]] Result<Manifest> parse_manifest(std::string_view text, ManifestLimits limits = {});

[[nodiscard]] Result<Manifest> load_manifest(
    const std::filesystem::path& path, ManifestLimits limits = {});

[[nodiscard]] bool valid_plugin_id(std::string_view value) noexcept;
[[nodiscard]] bool valid_plugin_name(std::string_view value) noexcept;

}  // namespace rodaq::plugin
