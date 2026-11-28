#pragma once

#include "../config.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::server {

Result<std::string> generate_random_hex(std::size_t byte_count);

bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

// Single high-entropy local Bearer token authenticator. Loaded once at startup
// from a controlled regular file (mode 0600 recommended) and compared in
// constant time; holds no session, credential database, or RBAC state.
class BearerAuthenticator {
public:
    static Result<std::unique_ptr<BearerAuthenticator>> create(const SecurityConfig& config);

    BearerAuthenticator(const BearerAuthenticator&) = delete;
    BearerAuthenticator& operator=(const BearerAuthenticator&) = delete;
    ~BearerAuthenticator() noexcept;

    bool authenticate(std::string_view bearer) const noexcept;

    std::uint64_t failure_count() const noexcept {
        return failures_.load(std::memory_order_relaxed);
    }

private:
    explicit BearerAuthenticator(std::string token);

    std::string token_;
    mutable std::atomic<std::uint64_t> failures_{0};
};

}  // namespace rodaq::server
