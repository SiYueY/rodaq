#include "artifact.hpp"

#include <rodaq/status.hpp>

#include <openssl/evp.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace rodaq::plugin {
namespace {

using EvpContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

std::string to_hex(const unsigned char* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output(size * 2u, '\0');
    for (std::size_t index = 0u; index < size; ++index) {
        output[index * 2u] = kHex[data[index] >> 4u];
        output[index * 2u + 1u] = kHex[data[index] & 0x0fu];
    }
    return output;
}

}  // namespace

bool valid_sha256(std::string_view value) noexcept {
    if (value.size() != 64u) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

Result<FileIdentity> file_identity(const std::filesystem::path& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        return Status{StatusCode::Unavailable, "artifact file cannot be inspected"};
    }
    return FileIdentity{
        static_cast<std::uint64_t>(status.st_dev), static_cast<std::uint64_t>(status.st_ino),
        static_cast<std::uint64_t>(status.st_size)};
}

bool same_file_identity(FileIdentity left, FileIdentity right) noexcept {
    return left.device == right.device && left.inode == right.inode && left.size == right.size;
}

Result<ArtifactDigest> verify_artifact(
    const std::filesystem::path& path, std::string_view expected_sha256, std::uint64_t max_size) {
    if (!valid_sha256(expected_sha256)) {
        return Status{StatusCode::InvalidArgument, "artifact digest must be lowercase SHA-256"};
    }

    auto identity_result = file_identity(path);
    if (!identity_result.ok()) {
        return identity_result.status();
    }
    const auto identity = identity_result.value();
    if (identity.size > max_size) {
        return Status{StatusCode::ResourceExhausted, "plugin artifact exceeds size limit"};
    }

    File file{std::fopen(path.c_str(), "rb"), &std::fclose};
    if (file == nullptr) {
        return Status{StatusCode::Unavailable, "plugin artifact cannot be opened"};
    }

    EvpContext context{EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (context == nullptr || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return Status{StatusCode::Internal, "SHA-256 context initialization failed"};
    }

    std::array<unsigned char, 64u * 1024u> buffer{};
    std::uint64_t total = 0u;
    while (true) {
        const auto count = std::fread(buffer.data(), 1u, buffer.size(), file.get());
        if (count > 0u) {
            total += count;
            if (total > max_size) {
                return Status{StatusCode::ResourceExhausted, "plugin artifact exceeds size limit"};
            }
            if (EVP_DigestUpdate(context.get(), buffer.data(), count) != 1) {
                return Status{StatusCode::Internal, "SHA-256 update failed"};
            }
        }
        if (count < buffer.size()) {
            if (std::ferror(file.get()) != 0) {
                return Status{StatusCode::Unavailable, "plugin artifact read failed"};
            }
            break;
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0u;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1 || digest_size != 32u) {
        return Status{StatusCode::Internal, "SHA-256 finalization failed"};
    }

    auto sha256 = to_hex(digest.data(), digest_size);
    if (sha256 != expected_sha256) {
        return Status{StatusCode::PermissionDenied, "plugin artifact digest mismatch"};
    }
    return ArtifactDigest{std::move(sha256), identity};
}

}  // namespace rodaq::plugin
