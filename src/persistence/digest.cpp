#include "json.hpp"
#include "digest.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace rodaq::persistence {
namespace {

std::string digest_text(const std::array<unsigned char, 32>& bytes) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string output = "sha256:";
    output.reserve(71);
    for (const auto byte : bytes) {
        output += kHex[byte >> 4u];
        output += kHex[byte & 0xfu];
    }
    return output;
}

}  // namespace

bool valid_digest(std::string_view value) noexcept {
    return value.size() == 71 && value.substr(0, 7) == "sha256:" &&
           std::all_of(value.begin() + 7, value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

Result<std::string> byte_digest(const void* data, std::size_t count) {
    if (data == nullptr && count != 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::array<unsigned char, 32> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(data, count, bytes.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != bytes.size()) {
        return Status(StatusCode::Internal, {});
    }
    try {
        return digest_text(bytes);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> file_digest(int descriptor, std::uint64_t expected_size) {
    static_assert(sizeof(off_t) >= 8);
    struct stat before {};
    if (fstat(descriptor, &before) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    if (!S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) != expected_size ||
        expected_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return Status(StatusCode::DataLoss, {});
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return Status(StatusCode::Internal, {});
    }
    std::array<unsigned char, 65536> buffer{};
    std::uint64_t offset = 0;
    while (offset < expected_size) {
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), expected_size - offset));
        const auto received =
            pread(descriptor, buffer.data(), requested, static_cast<off_t>(offset));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return Status(received == 0 ? StatusCode::DataLoss : StatusCode::Unavailable, {});
        }
        if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(received)) !=
            1) {
            return Status(StatusCode::Internal, {});
        }
        offset += static_cast<std::uint64_t>(received);
    }
    struct stat after {};
    if (fstat(descriptor, &after) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size || after.st_nlink != 1 ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        return Status(StatusCode::DataLoss, {});
    }
    std::array<unsigned char, 32> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), bytes.data(), &size) != 1 || size != bytes.size()) {
        return Status(StatusCode::Internal, {});
    }
    try {
        return digest_text(bytes);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> semantic_digest(
    const Json& value, std::string_view domain, const JsonLimits& limits) {
    if (domain.empty() || domain.size() > 128 ||
        !std::all_of(domain.begin(), domain.end(), [](unsigned char character) {
            return character >= 0x21 && character <= 0x7e;
        })) {
        return Status(StatusCode::InvalidArgument, "Invalid digest domain");
    }
    auto canonical = canonical_json(value, limits);
    if (!canonical.ok()) {
        return std::move(canonical).status();
    }
    try {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
            EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!context) {
            return Status(StatusCode::ResourceExhausted, "SHA-256 context allocation failed");
        }
        constexpr char kTerminator = '\0';
        std::array<unsigned char, 32> bytes{};
        unsigned int size = 0;
        if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(context.get(), domain.data(), domain.size()) != 1 ||
            EVP_DigestUpdate(context.get(), &kTerminator, 1) != 1 ||
            EVP_DigestUpdate(context.get(), canonical.value().data(), canonical.value().size()) !=
                1 ||
            EVP_DigestFinal_ex(context.get(), bytes.data(), &size) != 1 || size != bytes.size()) {
            return Status(StatusCode::Internal, "SHA-256 failed");
        }
        return digest_text(bytes);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> verify_integrity(
    const Json& document, std::string_view domain, const JsonLimits& limits) {
    if (!document.is_object()) {
        return Status(StatusCode::DataLoss, "Persistent document is not an object");
    }
    const auto found = document.find("content_digest");
    if (found == document.end() || !found->is_string() ||
        !valid_digest(found->get_ref<const std::string&>())) {
        return Status(StatusCode::DataLoss, "Invalid persistent digest field");
    }
    try {
        auto semantic = document;
        semantic.erase("content_digest");
        auto actual = semantic_digest(semantic, domain, limits);
        if (!actual.ok()) {
            return std::move(actual).status();
        }
        const auto& stored = found->get_ref<const std::string&>();
        if (CRYPTO_memcmp(stored.data(), actual.value().data(), stored.size()) != 0) {
            return Status(StatusCode::DataLoss, "Persistent content digest mismatch");
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> encode_integrity(
    const Json& document, std::string_view domain, const JsonLimits& limits) {
    if (!document.is_object()) {
        return Status(StatusCode::InvalidArgument, "Persistent document is not an object");
    }
    try {
        auto semantic = document;
        semantic.erase("content_digest");
        auto digest = semantic_digest(semantic, domain, limits);
        if (!digest.ok()) {
            return std::move(digest).status();
        }
        semantic["content_digest"] = std::move(digest).value();
        return canonical_json(semantic, limits);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::persistence
