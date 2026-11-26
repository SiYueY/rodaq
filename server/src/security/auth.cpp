#include "auth.hpp"

#include "../api/json_codec.hpp"

#include <algorithm>
#include <fstream>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <set>
#include <sstream>
#include <sys/stat.h>

namespace rodaq::server {
namespace {

constexpr std::string_view kBearerDomainPrefix("rodaq.server-bearer.v1\0", 23);

bool hex_nibble(char ch, std::uint8_t& out) noexcept {
    if (ch >= '0' && ch <= '9') {
        out = static_cast<std::uint8_t>(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        out = static_cast<std::uint8_t>(10 + (ch - 'a'));
        return true;
    }
    return false;
}

bool decode_lowercase_hex32(std::string_view hex, std::array<std::uint8_t, 32>& out) noexcept {
    if (hex.size() != 64) {
        return false;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        std::uint8_t hi = 0;
        std::uint8_t lo = 0;
        if (!hex_nibble(hex[i * 2], hi) || !hex_nibble(hex[i * 2 + 1], lo)) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

Result<std::array<std::uint8_t, 32>> hash_bearer_raw(std::string_view bearer) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return Status(StatusCode::ResourceExhausted, "failed to allocate EVP_MD_CTX");
    }
    struct Guard {
        EVP_MD_CTX* ptr;
        ~Guard() { EVP_MD_CTX_free(ptr); }
    } guard{ctx};

    std::array<std::uint8_t, 32> digest{};
    unsigned int len = 0;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, kBearerDomainPrefix.data(), kBearerDomainPrefix.size()) != 1 ||
        EVP_DigestUpdate(ctx, bearer.data(), bearer.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest.data(), &len) != 1 || len != 32) {
        return Status(StatusCode::Internal, "SHA-256 bearer hash failed");
    }
    return digest;
}

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

Result<std::vector<CredentialRecord>> load_credentials(const SecurityConfig& config) {
    struct stat st {};
    if (::lstat(config.credential_store.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return Status(StatusCode::NotFound, "credential store file missing or not regular file");
    }
    if (config.enforce_file_permissions && (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return Status(
            StatusCode::PermissionDenied,
            "credential store permissions must restrict group and other access");
    }
    if (st.st_size < 0 || static_cast<std::size_t>(st.st_size) > 1048576) {
        return Status(StatusCode::InvalidArgument, "credential store file too large");
    }
    std::ifstream file(config.credential_store, std::ios::binary);
    if (!file) {
        return Status(StatusCode::Unavailable, "unable to open credential store");
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const auto parsed = parse_strict_json(ss.str(), {1048576, 16, 4096, 2048, 16384});
    if (!parsed.ok) {
        return Status(StatusCode::InvalidArgument, parsed.error.message);
    }
    const auto& root = parsed.value;
    if (!root.is_object() || root.size() != 2 || !root.contains("format_version") ||
        !root["format_version"].is_string() ||
        root["format_version"].get_ref<const std::string&>() != "1.0" ||
        !root.contains("credentials") || !root["credentials"].is_array()) {
        return Status(StatusCode::InvalidArgument, "invalid credential store root schema");
    }
    const auto& arr = root["credentials"];
    if (arr.size() > config.max_credentials) {
        return Status(StatusCode::InvalidArgument, "credentials exceed max_credentials");
    }

    std::vector<CredentialRecord> records;
    records.reserve(arr.size());
    std::set<std::string> seen_ids;
    std::set<std::string> seen_principals;
    std::set<std::string> seen_verifiers;
    for (const auto& item : arr) {
        if (!item.is_object() || item.size() != 6 || !item.contains("credential_id") ||
            !item.contains("principal") || !item.contains("scopes") || !item.contains("verifier") ||
            !item.contains("credential_epoch") || !item.contains("enabled")) {
            return Status(StatusCode::InvalidArgument, "invalid credential record shape");
        }
        if (!item["credential_id"].is_string() || !item["principal"].is_string() ||
            !item["scopes"].is_array() || !item["verifier"].is_string() ||
            !item["credential_epoch"].is_string() || !item["enabled"].is_boolean()) {
            return Status(StatusCode::InvalidArgument, "invalid credential record field types");
        }
        CredentialRecord rec;
        rec.id = item["credential_id"].get<std::string>();
        rec.principal = item["principal"].get<std::string>();
        rec.enabled = item["enabled"].get<bool>();
        if (!is_visible_ascii(rec.id, 1, 128) || !is_visible_ascii(rec.principal, 1, 128)) {
            return Status(StatusCode::InvalidArgument, "invalid credential_id or principal format");
        }
        if (!seen_ids.insert(rec.id).second || !seen_principals.insert(rec.principal).second) {
            return Status(StatusCode::InvalidArgument, "duplicate credential_id or principal");
        }
        const auto& verifier_hex = item["verifier"].get_ref<const std::string&>();
        if (!decode_lowercase_hex32(verifier_hex, rec.verifier) ||
            !seen_verifiers.insert(verifier_hex).second) {
            return Status(StatusCode::InvalidArgument, "invalid or duplicate credential verifier");
        }
        const auto epoch =
            parse_u64_decimal(item["credential_epoch"].get_ref<const std::string&>());
        if (!epoch || *epoch == 0) {
            return Status(StatusCode::InvalidArgument, "credential_epoch must be nonzero decimal");
        }
        rec.credential_epoch = *epoch;
        std::set<std::string> seen_scopes;
        for (const auto& scope_val : item["scopes"]) {
            if (!scope_val.is_string()) {
                return Status(StatusCode::InvalidArgument, "scope must be string");
            }
            const auto& scope = scope_val.get_ref<const std::string&>();
            if (!seen_scopes.insert(scope).second) {
                return Status(StatusCode::InvalidArgument, "duplicate scope in credential");
            }
            if (scope == "read") {
                rec.scopes.add(Scope::Read);
            } else if (scope == "control") {
                rec.scopes.add(Scope::Control);
            } else if (scope == "admin") {
                rec.scopes.add(Scope::Admin);
            } else {
                return Status(StatusCode::InvalidArgument, "unknown scope in credential");
            }
        }
        if (rec.scopes.empty()) {
            return Status(StatusCode::InvalidArgument, "credential scopes must be non-empty");
        }
        records.push_back(std::move(rec));
    }
    return records;
}

}  // namespace

std::vector<std::string> ScopeSet::to_wire() const {
    std::vector<std::string> out;
    if (has(Scope::Read)) {
        out.emplace_back("read");
    }
    if (has(Scope::Control)) {
        out.emplace_back("control");
    }
    if (has(Scope::Admin)) {
        out.emplace_back("admin");
    }
    return out;
}

Result<std::string> compute_bearer_verifier_hex(std::string_view bearer) {
    auto raw = hash_bearer_raw(bearer);
    if (!raw.ok()) {
        return raw.status();
    }
    return to_lowercase_hex(raw.value().data(), raw.value().size());
}

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

AuthState::AuthState(SecurityConfig config, std::vector<CredentialRecord> credentials)
: config_(std::move(config)), credentials_(std::move(credentials)) {}

Result<std::unique_ptr<AuthState>> AuthState::create(const SecurityConfig& config) {
    auto loaded = load_credentials(config);
    if (!loaded.ok()) {
        return loaded.status();
    }
    return std::unique_ptr<AuthState>(new AuthState(config, std::move(loaded.value())));
}

const CredentialRecord* AuthState::find_matching_credential(std::string_view bearer) const {
    // Reject empty or obviously low-entropy bearers before hashing.
    if (bearer.size() < 32 || bearer.size() > 512) {
        return nullptr;
    }
    auto digest = hash_bearer_raw(bearer);
    if (!digest.ok()) {
        return nullptr;
    }
    const CredentialRecord* matched = nullptr;
    // Always scan all credentials using constant-time byte comparison.
    for (const auto& cred : credentials_) {
        const bool same =
            CRYPTO_memcmp(cred.verifier.data(), digest.value().data(), cred.verifier.size()) == 0;
        if (same && cred.enabled) {
            matched = &cred;
        }
    }
    return matched;
}

bool AuthState::credential_epoch_valid(
    const std::string& principal, std::uint64_t epoch) const noexcept {
    for (const auto& cred : credentials_) {
        if (cred.principal == principal) {
            return cred.enabled && cred.credential_epoch == epoch;
        }
    }
    return false;
}

void AuthState::purge_expired_locked(std::chrono::steady_clock::time_point now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const bool idle_expired = now - it->second.last_activity >= config_.session_idle_timeout;
        const bool abs_expired = now >= it->second.absolute_expiry;
        const bool epoch_ok = credential_epoch_valid(
            it->second.snapshot.principal, it->second.snapshot.credential_epoch);
        if (idle_expired || abs_expired || !epoch_ok) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

Result<AuthPrincipal> AuthState::authenticate_bearer(
    std::string_view bearer, ApiVersion negotiated_version) const {
    const auto* cred = find_matching_credential(bearer);
    if (!cred) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++login_failures_;
        return Status(StatusCode::PermissionDenied, "invalid bearer credential");
    }
    AuthPrincipal principal;
    principal.principal = cred->principal;
    principal.scopes = cred->scopes;
    principal.api_version = negotiated_version;
    principal.via_session = false;
    return principal;
}

Result<SessionSnapshot> AuthState::create_session(
    std::string_view bearer, ApiVersion negotiated_version) {
    const auto* cred = find_matching_credential(bearer);
    if (!cred) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++login_failures_;
        return Status(StatusCode::PermissionDenied, "invalid bearer credential");
    }
    auto sid = generate_random_hex(32);
    auto csrf = generate_random_hex(32);
    if (!sid.ok() || !csrf.ok()) {
        return Status(StatusCode::Unavailable, "failed to generate session tokens");
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    purge_expired_locked(now);
    if (sessions_.size() >= config_.max_sessions) {
        return Status(StatusCode::ResourceExhausted, "active session capacity exhausted");
    }
    SessionEntry entry;
    entry.snapshot.session_id = std::move(sid.value());
    entry.snapshot.principal = cred->principal;
    entry.snapshot.scopes = cred->scopes;
    entry.snapshot.api_version = negotiated_version;
    entry.snapshot.csrf_token = std::move(csrf.value());
    entry.snapshot.credential_epoch = cred->credential_epoch;
    entry.created_at = now;
    entry.last_activity = now;
    entry.absolute_expiry = now + config_.session_absolute_timeout;

    SessionSnapshot copy = entry.snapshot;
    sessions_.emplace(copy.session_id, std::move(entry));
    high_water_sessions_ = std::max(high_water_sessions_, sessions_.size());
    return copy;
}

Result<AuthPrincipal> AuthState::authenticate_session(std::string_view session_id) {
    if (session_id.empty() || session_id.size() > 128) {
        return Status(StatusCode::PermissionDenied, "missing or invalid session cookie");
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(std::string(session_id));
    if (it == sessions_.end()) {
        return Status(StatusCode::PermissionDenied, "session not found");
    }
    const bool idle_expired = now - it->second.last_activity >= config_.session_idle_timeout;
    const bool abs_expired = now >= it->second.absolute_expiry;
    const bool epoch_ok =
        credential_epoch_valid(it->second.snapshot.principal, it->second.snapshot.credential_epoch);
    if (idle_expired || abs_expired || !epoch_ok) {
        sessions_.erase(it);
        return Status(StatusCode::PermissionDenied, "session expired or revoked");
    }
    // Activity updates never extend absolute_expiry.
    it->second.last_activity = now;
    AuthPrincipal principal;
    principal.principal = it->second.snapshot.principal;
    principal.scopes = it->second.snapshot.scopes;
    principal.api_version = it->second.snapshot.api_version;
    principal.via_session = true;
    principal.session_id = it->second.snapshot.session_id;
    principal.csrf_token = it->second.snapshot.csrf_token;
    return principal;
}

Result<SessionSnapshot> AuthState::get_session(std::string_view session_id) {
    auto auth = authenticate_session(session_id);
    if (!auth.ok()) {
        return auth.status();
    }
    SessionSnapshot snap;
    snap.session_id = std::move(auth.value().session_id);
    snap.principal = std::move(auth.value().principal);
    snap.scopes = auth.value().scopes;
    snap.api_version = auth.value().api_version;
    snap.csrf_token = std::move(auth.value().csrf_token);
    return snap;
}

void AuthState::destroy_session(std::string_view session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(std::string(session_id));
}

std::size_t AuthState::active_session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::size_t AuthState::high_water_sessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return high_water_sessions_;
}

std::uint64_t AuthState::login_failure_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return login_failures_;
}

}  // namespace rodaq::server
