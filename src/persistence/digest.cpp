#include "digest.hpp"

#include <array>
#include <new>
#include <string>
#include <string_view>

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

}  // namespace rodaq::persistence
