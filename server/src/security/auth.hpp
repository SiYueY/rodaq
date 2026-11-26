#pragma once

#include "config.hpp"

#include <array>
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

namespace rodaq::server {

enum class Scope : std::uint8_t { Read = 1u << 0, Control = 1u << 1, Admin = 1u << 2 };

struct ScopeSet {
    std::uint8_t mask{0};

    bool has(Scope scope) const noexcept { return (mask & static_cast<std::uint8_t>(scope)) != 0; }

    void add(Scope scope) noexcept { mask |= static_cast<std::uint8_t>(scope); }

    bool empty() const noexcept { return mask == 0; }

    std::vector<std::string> to_wire() const;
};

struct ApiVersion {
    std::uint32_t major{1};
    std::uint32_t minor{0};
};

struct CredentialRecord {
    std::string id;
    std::string principal;
    ScopeSet scopes;
    std::array<std::uint8_t, 32> verifier{};
    std::uint64_t credential_epoch{1};
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

struct AuthPrincipal {
    std::string principal;
    ScopeSet scopes;
    ApiVersion api_version;
    bool via_session{false};
    std::string session_id;
    std::string csrf_token;
};

// Computes SHA-256("rodaq.server-bearer.v1\0" || bearer_bytes) as 64 lowercase hex chars.
Result<std::string> compute_bearer_verifier_hex(std::string_view bearer);

Result<std::string> generate_random_hex(std::size_t byte_count);

bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

// Owns startup-loaded immutable credentials and bounded process-local sessions.
// Never stores Task/Operation authority. Thread-safe.
class AuthState {
public:
    static Result<std::unique_ptr<AuthState>> create(const SecurityConfig& config);

    AuthState(const AuthState&) = delete;
    AuthState& operator=(const AuthState&) = delete;
    ~AuthState() noexcept = default;

    Result<AuthPrincipal> authenticate_bearer(
        std::string_view bearer, ApiVersion negotiated_version) const;

    Result<SessionSnapshot> create_session(std::string_view bearer, ApiVersion negotiated_version);

    Result<AuthPrincipal> authenticate_session(std::string_view session_id);

    Result<SessionSnapshot> get_session(std::string_view session_id);

    void destroy_session(std::string_view session_id);

    std::size_t active_session_count() const;

    std::size_t high_water_sessions() const;

    std::uint64_t login_failure_count() const;

private:
    struct SessionEntry {
        SessionSnapshot snapshot;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_activity;
        std::chrono::steady_clock::time_point absolute_expiry;
    };

    AuthState(SecurityConfig config, std::vector<CredentialRecord> credentials);

    void purge_expired_locked(std::chrono::steady_clock::time_point now);
    const CredentialRecord* find_matching_credential(std::string_view bearer) const;
    bool credential_epoch_valid(const std::string& principal, std::uint64_t epoch) const noexcept;

    SecurityConfig config_;
    std::vector<CredentialRecord> credentials_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SessionEntry> sessions_;
    std::size_t high_water_sessions_{0};
    mutable std::uint64_t login_failures_{0};
};

}  // namespace rodaq::server
