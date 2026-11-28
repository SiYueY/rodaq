#include "auth.hpp"

#include "../api/json_codec.hpp"

#include <fstream>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace rodaq::server {
namespace {

std::string to_lowercase_hex(const std::uint8_t* bytes, std::size_t count) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0Fu];
    }
    return out;
}

std::string trim_trailing_newlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

}  // namespace

Result<std::string> generate_random_hex(std::size_t byte_count) {
    if (byte_count == 0 || byte_count > 64) {
        return Status(StatusCode::InvalidArgument, "invalid random byte count");
    }
    std::vector<std::uint8_t> bytes(byte_count);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return Status(StatusCode::Unavailable, "CSPRNG failure");
    }
    return to_lowercase_hex(bytes.data(), bytes.size());
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    return a.empty() || CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

BearerAuthenticator::BearerAuthenticator(std::string token) : token_(std::move(token)) {}

BearerAuthenticator::~BearerAuthenticator() noexcept {
    if (!token_.empty()) {
        OPENSSL_cleanse(token_.data(), token_.size());
    }
}

Result<std::unique_ptr<BearerAuthenticator>> BearerAuthenticator::create(
    const SecurityConfig& config) {
    struct stat st {};
    if (::lstat(config.bearer_token_file.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return Status(StatusCode::NotFound, "bearer token file missing or not regular file");
    }
    if (config.enforce_file_permissions && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return Status(
            StatusCode::PermissionDenied,
            "bearer token file permissions must restrict group and other access");
    }
    if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > 4096) {
        return Status(StatusCode::InvalidArgument, "bearer token file too large");
    }
    std::ifstream file(config.bearer_token_file, std::ios::binary);
    if (!file) {
        return Status(StatusCode::Unavailable, "unable to open bearer token file");
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string token = trim_trailing_newlines(ss.str());
    if (!is_visible_ascii(token, 32, 512)) {
        return Status(
            StatusCode::InvalidArgument, "bearer token must be 32..512 visible ASCII characters");
    }
    return std::unique_ptr<BearerAuthenticator>(new BearerAuthenticator(std::move(token)));
}

bool BearerAuthenticator::authenticate(std::string_view bearer) const noexcept {
    if (bearer.size() < 32 || bearer.size() > 512) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!constant_time_equal(bearer, token_)) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

}  // namespace rodaq::server
