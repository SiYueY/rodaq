#include "json.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <string>
#include <string_view>

#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace rodaq::persistence {

bool valid_digest(std::string_view value) noexcept {
    return value.size() == 71 && value.substr(0, 7) == "sha256:" &&
           std::all_of(value.begin() + 7, value.end(), [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
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
        return canonical.status();
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
        constexpr std::string_view kHex = "0123456789abcdef";
        std::string output = "sha256:";
        output.reserve(71);
        for (const auto byte : bytes) {
            output += kHex[byte >> 4u];
            output += kHex[byte & 0xfu];
        }
        return output;
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
            return actual.status();
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
            return digest.status();
        }
        semantic["content_digest"] = std::move(digest).value();
        return canonical_json(semantic, limits);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::persistence
