#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rodaq/result.hpp>

#include "../server_config.hpp"

namespace rodaq::server {

struct ApiVersion {
    std::uint32_t major{1};
    std::uint32_t minor{0};

    friend bool operator==(ApiVersion a, ApiVersion b) noexcept {
        return a.major == b.major && a.minor == b.minor;
    }
};

enum class Scope : std::uint8_t {
    Read = 1u << 0,
    Control = 1u << 1,
    Admin = 1u << 2,
};

struct ScopeSet {
    std::uint8_t mask{0};

    bool has(Scope s) const noexcept { return (mask & static_cast<std::uint8_t>(s)) != 0; }
    void add(Scope s) noexcept { mask |= static_cast<std::uint8_t>(s); }
    bool empty() const noexcept { return mask == 0; }
    std::vector<std::string> names() const;
};

struct CredentialRecord {
    std::string id;
    std::string principal;
    ScopeSet scopes;
    std::string verifier_hex;
    std::uint64_t credential_epoch{0};
    bool enabled{true};
};

struct SessionSnapshot {
    std::string session_id;
    std::string principal;
    ScopeSet scopes;
    ApiVersion api_version;
    std::string csrf_token;
    std::uint64_t credential_epoch{0};
};

struct AuthContext {
    std::string principal;
    ScopeSet scopes;
    ApiVersion api_version;
    bool via_session_cookie{false};
    std::string session_id;
    std::string csrf_token;
};

// Computes SHA-256("rodaq.server-bearer.v1\0" || bearer_bytes) as 64 lowercase hex characters.
Result<std::string> compute_bearer_verifier(std::string_view bearer_bytes);

// Generates 256-bit CSPRNG hex string (64 lowercase hex chars).
Result<std::string> generate_secure_token();

class AuthState {
public:
    static Result<std::unique_ptr<AuthState>> create(const SecurityConfig& config);

    AuthState(const AuthState&) = delete;
    AuthState& operator=(const AuthState&) = delete;
    ~AuthState() noexcept = default;

    Result<AuthContext> authenticate_bearer(
        std::string_view bearer_token, ApiVersion api_version) const;
    Result<SessionSnapshot> create_session(std::string_view bearer_token, ApiVersion api_version);
    Result<AuthContext> authenticate_session(std::string_view session_id);
    bool verify_csrf(const AuthContext& auth, std::string_view presented_csrf) const noexcept;
    void destroy_session(std::string_view session_id) noexcept;
    std::size_t active_sessions() const noexcept;

private:
    struct SessionEntry {
        SessionSnapshot snapshot;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_activity;
        std::chrono::steady_clock::time_point absolute_expiry;
    };

    AuthState(SecurityConfig config, std::vector<CredentialRecord> credentials) noexcept;

    void purge_expired_locked(std::chrono::steady_clock::time_point now) noexcept;
    const CredentialRecord* find_matching_credential_const(
        std::string_view bearer_token) const noexcept;
    bool credential_epoch_valid_locked(
        const std::string& principal, std::uint64_t epoch) const noexcept;

    SecurityConfig config_;
    std::vector<CredentialRecord> credentials_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionEntry> sessions_;
};

}  // namespace rodaq::server
