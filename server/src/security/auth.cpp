#include "auth.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace rodaq::server {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kBearerDomainPrefix{"rodaq.server-bearer.v1\0", 23};
constexpr std::size_t kMinBearerBytes = 32;
constexpr std::size_t kVerifierHexLength = 64;

bool is_visible_ascii(std::string_view s, std::size_t min_len, std::size_t max_len) noexcept {
    if (s.size() < min_len || s.size() > max_len) {
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool is_lower_hex_64(std::string_view s) noexcept {
    if (s.size() != kVerifierHexLength) {
        return false;
    }
    for (unsigned char c : s) {
        const bool digit = c >= '0' && c <= '9';
        const bool hex = c >= 'a' && c <= 'f';
        if (!digit && !hex) {
            return false;
        }
    }
    return true;
}

std::string bytes_to_lower_hex(const unsigned char* data, std::size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[data[i] & 0x0Fu]);
    }
    return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

Result<Json> parse_strict_credentials_json(std::string_view text) {
    struct DuplicateTracker {
        std::vector<std::set<std::string>> stack;
        bool duplicate{false};
    } tracker;

    auto cb = [&tracker](int /*depth*/, Json::parse_event_t ev, Json& parsed) -> bool {
        if (ev == Json::parse_event_t::object_start) {
            tracker.stack.emplace_back();
        } else if (ev == Json::parse_event_t::object_end) {
            if (!tracker.stack.empty()) {
                tracker.stack.pop_back();
            }
        } else if (ev == Json::parse_event_t::key) {
            if (!tracker.stack.empty()) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!tracker.stack.back().insert(key).second) {
                    tracker.duplicate = true;
                    return false;
                }
            }
        }
        return true;
    };

    try {
        auto parsed = Json::parse(text.begin(), text.end(), cb, true, false);
        if (tracker.duplicate || parsed.is_discarded()) {
            return Status(StatusCode::InvalidArgument, "Invalid credential store JSON");
        }
        return parsed;
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed credential store JSON");
    }
}

Result<std::uint64_t> parse_nonzero_epoch(const Json& val) {
    if (!val.is_string()) {
        return Status(StatusCode::InvalidArgument, "credential_epoch must be decimal string");
    }
    const auto& s = val.get_ref<const std::string&>();
    if (s.empty() || s.size() > 20 || s[0] == '0') {
        return Status(StatusCode::InvalidArgument, "credential_epoch must be nonzero decimal");
    }
    std::uint64_t out = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || ptr != s.data() + s.size() || out == 0) {
        return Status(StatusCode::InvalidArgument, "Invalid credential_epoch");
    }
    return out;
}

}  // namespace

std::vector<std::string> ScopeSet::names() const {
    std::vector<std::string> result;
    if (has(Scope::Read)) {
        result.emplace_back("read");
    }
    if (has(Scope::Control)) {
        result.emplace_back("control");
    }
    if (has(Scope::Admin)) {
        result.emplace_back("admin");
    }
    return result;
}

Result<std::string> compute_bearer_verifier(std::string_view bearer_bytes) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return Status(StatusCode::ResourceExhausted, "EVP_MD_CTX_new failed");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    const bool ok =
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, kBearerDomainPrefix.data(), kBearerDomainPrefix.size()) == 1 &&
        EVP_DigestUpdate(ctx, bearer_bytes.data(), bearer_bytes.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok || digest_len != 32) {
        return Status(StatusCode::Internal, "SHA-256 computation failed");
    }
    return bytes_to_lower_hex(digest.data(), digest_len);
}

Result<std::string> generate_secure_token() {
    std::array<unsigned char, 32> buf{};
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
        return Status(StatusCode::Unavailable, "CSPRNG unavailable");
    }
    return bytes_to_lower_hex(buf.data(), buf.size());
}

AuthState::AuthState(SecurityConfig config, std::vector<CredentialRecord> credentials) noexcept
: config_(std::move(config)), credentials_(std::move(credentials)) {}

Result<std::unique_ptr<AuthState>> AuthState::create(const SecurityConfig& config) {
    struct stat st {};
    if (::lstat(config.credential_store.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return Status(StatusCode::InvalidArgument, "Credential store must be a regular file");
    }
    if (config.enforce_credential_file_mode && ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)) {
        return Status(
            StatusCode::PermissionDenied,
            "Credential store permissions must not expose group/other access");
    }
    if (st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) {
        return Status(StatusCode::InvalidArgument, "Credential store size out of bounds");
    }

    std::ifstream in(config.credential_store, std::ios::binary);
    if (!in.good()) {
        return Status(StatusCode::Unavailable, "Failed to open credential store");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const auto raw = ss.str();

    auto parsed = parse_strict_credentials_json(raw);
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto& root = parsed.value();
    if (!root.is_object()) {
        return Status(StatusCode::InvalidArgument, "Credential store root must be object");
    }
    for (const auto& [k, _] : root.items()) {
        if (k != "format_version" && k != "credentials") {
            return Status(StatusCode::InvalidArgument, "Unknown credential store field");
        }
    }
    if (!root.contains("format_version") || !root["format_version"].is_string() ||
        root["format_version"].get<std::string>() != "1.0" || !root.contains("credentials") ||
        !root["credentials"].is_array()) {
        return Status(StatusCode::InvalidArgument, "Invalid credential store header");
    }

    const auto& arr = root["credentials"];
    if (arr.empty() || arr.size() > config.max_credentials) {
        return Status(StatusCode::InvalidArgument, "Credential count out of bounds");
    }

    std::vector<CredentialRecord> records;
    records.reserve(arr.size());
    std::set<std::string> seen_ids;
    std::set<std::string> seen_verifiers;

    for (const auto& item : arr) {
        if (!item.is_object()) {
            return Status(StatusCode::InvalidArgument, "Credential item must be object");
        }
        for (const auto& [k, _] : item.items()) {
            if (k != "credential_id" && k != "principal" && k != "scopes" && k != "verifier" &&
                k != "credential_epoch" && k != "enabled") {
                return Status(StatusCode::InvalidArgument, "Unknown credential item field");
            }
        }
        if (!item.contains("credential_id") || !item.contains("principal") ||
            !item.contains("scopes") || !item.contains("verifier") ||
            !item.contains("credential_epoch") || !item.contains("enabled") ||
            !item["credential_id"].is_string() || !item["principal"].is_string() ||
            !item["scopes"].is_array() || !item["verifier"].is_string() ||
            !item["enabled"].is_boolean()) {
            return Status(StatusCode::InvalidArgument, "Missing or invalid credential item fields");
        }

        CredentialRecord rec;
        rec.id = item["credential_id"].get<std::string>();
        rec.principal = item["principal"].get<std::string>();
        rec.verifier_hex = item["verifier"].get<std::string>();
        rec.enabled = item["enabled"].get<bool>();

        if (!is_visible_ascii(rec.id, 1, 128) || !is_visible_ascii(rec.principal, 1, 128) ||
            !is_lower_hex_64(rec.verifier_hex)) {
            return Status(StatusCode::InvalidArgument, "Invalid credential identifier or verifier");
        }
        if (!seen_ids.insert(rec.id).second || !seen_verifiers.insert(rec.verifier_hex).second) {
            return Status(StatusCode::InvalidArgument, "Duplicate credential_id or verifier");
        }

        auto epoch = parse_nonzero_epoch(item["credential_epoch"]);
        if (!epoch.ok()) {
            return epoch.status();
        }
        rec.credential_epoch = epoch.value();

        const auto& scopes_json = item["scopes"];
        if (scopes_json.empty()) {
            return Status(StatusCode::InvalidArgument, "Credential scopes cannot be empty");
        }
        std::set<std::string> seen_scopes;
        for (const auto& s : scopes_json) {
            if (!s.is_string()) {
                return Status(StatusCode::InvalidArgument, "Scope must be string");
            }
            const auto& name = s.get_ref<const std::string&>();
            if (!seen_scopes.insert(name).second) {
                return Status(StatusCode::InvalidArgument, "Duplicate scope");
            }
            if (name == "read") {
                rec.scopes.add(Scope::Read);
            } else if (name == "control") {
                rec.scopes.add(Scope::Control);
            } else if (name == "admin") {
                rec.scopes.add(Scope::Admin);
            } else {
                return Status(StatusCode::InvalidArgument, "Unknown scope: " + name);
            }
        }
        records.push_back(std::move(rec));
    }

    return std::unique_ptr<AuthState>(new AuthState(config, std::move(records)));
}

const CredentialRecord* AuthState::find_matching_credential_const(
    std::string_view bearer_token) const noexcept {
    if (bearer_token.size() < kMinBearerBytes || bearer_token.size() > 1024) {
        return nullptr;
    }
    auto computed = compute_bearer_verifier(bearer_token);
    if (!computed.ok()) {
        return nullptr;
    }
    const CredentialRecord* matched = nullptr;
    for (const auto& cred : credentials_) {
        if (constant_time_equal(computed.value(), cred.verifier_hex) && cred.enabled) {
            matched = &cred;
        }
    }
    return matched;
}

Result<AuthContext> AuthState::authenticate_bearer(
    std::string_view bearer_token, ApiVersion api_version) const {
    const auto* cred = find_matching_credential_const(bearer_token);
    if (!cred) {
        return Status(StatusCode::PermissionDenied, "Unauthenticated");
    }
    AuthContext ctx;
    ctx.principal = cred->principal;
    ctx.scopes = cred->scopes;
    ctx.api_version = api_version;
    ctx.via_session_cookie = false;
    return ctx;
}

void AuthState::purge_expired_locked(std::chrono::steady_clock::time_point now) noexcept {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        const auto& s = it->second;
        const bool expired =
            now >= s.absolute_expiry || (now - s.last_activity) >= config_.session_idle_timeout ||
            !credential_epoch_valid_locked(s.snapshot.principal, s.snapshot.credential_epoch);
        if (expired) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

bool AuthState::credential_epoch_valid_locked(
    const std::string& principal, std::uint64_t epoch) const noexcept {
    return std::any_of(credentials_.begin(), credentials_.end(), [&](const auto& c) {
        return c.enabled && c.principal == principal && c.credential_epoch == epoch;
    });
}

Result<SessionSnapshot> AuthState::create_session(
    std::string_view bearer_token, ApiVersion api_version) {
    const auto* cred = find_matching_credential_const(bearer_token);
    if (!cred) {
        return Status(StatusCode::PermissionDenied, "Unauthenticated");
    }
    auto sid = generate_secure_token();
    if (!sid.ok()) {
        return sid.status();
    }
    auto csrf = generate_secure_token();
    if (!csrf.ok()) {
        return csrf.status();
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    purge_expired_locked(now);
    if (sessions_.size() >= config_.max_sessions) {
        return Status(StatusCode::ResourceExhausted, "Session capacity exhausted");
    }

    SessionSnapshot snap;
    snap.session_id = std::move(sid.value());
    snap.principal = cred->principal;
    snap.scopes = cred->scopes;
    snap.api_version = api_version;
    snap.csrf_token = std::move(csrf.value());
    snap.credential_epoch = cred->credential_epoch;

    SessionEntry entry;
    entry.snapshot = snap;
    entry.created_at = now;
    entry.last_activity = now;
    entry.absolute_expiry = now + config_.session_absolute_timeout;

    sessions_.emplace(snap.session_id, std::move(entry));
    return snap;
}

Result<AuthContext> AuthState::authenticate_session(std::string_view session_id) {
    if (session_id.size() != 64) {
        return Status(StatusCode::PermissionDenied, "Invalid session");
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(std::string(session_id));
    if (it == sessions_.end()) {
        return Status(StatusCode::PermissionDenied, "Unknown session");
    }
    auto& entry = it->second;
    const bool expired =
        now >= entry.absolute_expiry ||
        (now - entry.last_activity) >= config_.session_idle_timeout ||
        !credential_epoch_valid_locked(entry.snapshot.principal, entry.snapshot.credential_epoch);
    if (expired) {
        sessions_.erase(it);
        return Status(StatusCode::PermissionDenied, "Session expired");
    }
    entry.last_activity = now;

    AuthContext ctx;
    ctx.principal = entry.snapshot.principal;
    ctx.scopes = entry.snapshot.scopes;
    ctx.api_version = entry.snapshot.api_version;
    ctx.via_session_cookie = true;
    ctx.session_id = entry.snapshot.session_id;
    ctx.csrf_token = entry.snapshot.csrf_token;
    return ctx;
}

bool AuthState::verify_csrf(
    const AuthContext& auth, std::string_view presented_csrf) const noexcept {
    if (!auth.via_session_cookie) {
        return true;
    }
    return constant_time_equal(auth.csrf_token, presented_csrf);
}

void AuthState::destroy_session(std::string_view session_id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(std::string(session_id));
}

std::size_t AuthState::active_sessions() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace rodaq::server
