#pragma once

#include <rodaq/result.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace rodaq::plugin {

struct FileIdentity final {
    std::uint64_t device = 0u;
    std::uint64_t inode = 0u;
    std::uint64_t size = 0u;
};

struct ArtifactDigest final {
    std::string sha256;
    FileIdentity identity;
};

[[nodiscard]] Result<ArtifactDigest> verify_artifact(
    const std::filesystem::path& path, std::string_view expected_sha256, std::uint64_t max_size);

[[nodiscard]] Result<FileIdentity> file_identity(const std::filesystem::path& path);
[[nodiscard]] bool same_file_identity(FileIdentity left, FileIdentity right) noexcept;
[[nodiscard]] bool valid_sha256(std::string_view value) noexcept;

}  // namespace rodaq::plugin
