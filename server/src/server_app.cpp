#include "server_app.hpp"

#include "api/dto.hpp"
#include "api/json_codec.hpp"

#include <drogon/drogon.h>
#include <rodaq/logging.hpp>

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace rodaq::server {
namespace {

constexpr const char* kSessionCookieName = "rodaq_session";
constexpr const char* kCspHeaderValue =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
    "connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'";

void apply_common_headers(
    const drogon::HttpResponsePtr& resp, const std::string& request_id,
    ApiVersion version = {kApiMajor, kApiMinor}) {
    resp->addHeader(
        "RoDAQ-API-Version", std::to_string(version.major) + "." + std::to_string(version.minor));
    resp->addHeader("RoDAQ-Request-ID", request_id);
    resp->addHeader("Content-Security-Policy", kCspHeaderValue);
    resp->addHeader("Referrer-Policy", "no-referrer");
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("X-Frame-Options", "DENY");
}

drogon::HttpResponsePtr make_json_response(
    int status_code, const nlohmann::json& body, const std::string& request_id,
    std::uint64_t max_response_bytes, ApiVersion version = {kApiMajor, kApiMinor},
    std::optional<std::uint32_t> retry_after = std::nullopt) {
    std::string serialized = body.dump();
    if (serialized.size() > max_response_bytes) {
        ApiError too_large = make_request_error(
            500, "response_too_large", "serialized JSON response exceeds configured limit");
        auto fallback = make_error_envelope(request_id, std::move(too_large), version).dump();
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(500));
        resp->setContentTypeCodeAndCustomString(
            drogon::CT_CUSTOM, "application/json; charset=utf-8");
        resp->setBody(std::move(fallback));
        resp->addHeader("Cache-Control", "no-store");
        apply_common_headers(resp, request_id, version);
        return resp;
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status_code));
    resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM, "application/json; charset=utf-8");
    resp->setBody(std::move(serialized));
    resp->addHeader("Cache-Control", "no-store");
    if (retry_after) {
        resp->addHeader("Retry-After", std::to_string(*retry_after));
    }
    apply_common_headers(resp, request_id, version);
    return resp;
}

drogon::HttpResponsePtr make_error_http(
    const ApiError& error, const std::string& request_id, std::uint64_t max_response_bytes,
    ApiVersion version = {kApiMajor, kApiMinor}) {
    auto body = make_error_envelope(request_id, error, version);
    return make_json_response(
        error.http_status, body, request_id, max_response_bytes, version,
        error.retry_after_seconds);
}

std::optional<ApiVersion> parse_api_version_header(std::string_view raw) noexcept {
    const auto dot = raw.find('.');
    if (dot == std::string_view::npos || raw.find('.', dot + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto maj = parse_u64_decimal(raw.substr(0, dot));
    const auto min = parse_u64_decimal(raw.substr(dot + 1));
    if (!maj || !min || *maj > 100000 || *min > 100000) {
        return std::nullopt;
    }
    return ApiVersion{static_cast<std::uint32_t>(*maj), static_cast<std::uint32_t>(*min)};
}

std::optional<std::string_view> extract_bearer_token(std::string_view auth_header) noexcept {
    constexpr std::string_view kPrefix = "Bearer ";
    if (auth_header.size() <= kPrefix.size() || auth_header.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    return auth_header.substr(kPrefix.size());
}

bool content_type_is_json(std::string_view ct) noexcept {
    // Allow "application/json" with optional parameters like "; charset=utf-8".
    const auto semi = ct.find(';');
    std::string_view media = semi == std::string_view::npos ? ct : ct.substr(0, semi);
    while (!media.empty() && (media.front() == ' ' || media.front() == '\t')) {
        media.remove_prefix(1);
    }
    while (!media.empty() && (media.back() == ' ' || media.back() == '\t')) {
        media.remove_suffix(1);
    }
    if (media.size() != 16) {
        return false;
    }
    static constexpr std::string_view kJson = "application/json";
    for (std::size_t i = 0; i < 16; ++i) {
        char c = media[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != kJson[i]) {
            return false;
        }
    }
    return true;
}

std::size_t total_header_bytes(const drogon::HttpRequestPtr& req) noexcept {
    std::size_t total = 0;
    for (const auto& kv : req->headers()) {
        total += kv.first.size() + kv.second.size() + 4;
    }
    return total;
}

void log_audit(
    std::string_view event, std::string_view request_id, std::string_view principal,
    std::string_view detail = {}) {
    RODAQ_LOG_INFO(
        "audit event={} request_id={} principal={} detail={}", event, request_id,
        principal.empty() ? "-" : principal, detail.empty() ? "-" : detail);
}

struct RequestGuardResult {
    bool ok{false};
    std::string request_id;
    AuthPrincipal principal;
    drogon::HttpResponsePtr rejection;
};

struct GuardOptions {
    bool require_auth{true};
    bool allow_bearer{true};
    bool allow_session{true};
    bool is_mutation{false};
    bool expect_json_body{false};
    bool allow_in_recovering{false};
    bool allow_in_draining{false};
    std::optional<Scope> required_scope;
};

}  // namespace

struct ServerApp::Impl {
    ServerConfig config;
    mutable std::mutex services_mutex;
    ServerServices services;
    std::atomic<ServerPhase> phase{ServerPhase::Recovering};
    std::unique_ptr<AuthState> auth;
    std::unique_ptr<ServiceCallExecutor> executor;
    std::unique_ptr<DownloadRegistry> downloads;
    StaticAssetService static_assets;
    std::thread drogon_thread;
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};

    ServerServices current_services() const {
        std::lock_guard<std::mutex> lock(services_mutex);
        return services;
    }

    StrictJsonLimits json_limits() const noexcept {
        return {
            static_cast<std::size_t>(config.http.max_request_body_bytes),
            config.http.max_json_depth, static_cast<std::size_t>(config.http.max_json_string_bytes),
            config.http.max_json_members, 65536};
    }

    std::string new_request_id() const {
        auto id = generate_random_hex(16);
        return id.ok() ? id.value() : "00000000000000000000000000000000";
    }

    RequestGuardResult guard_request(const drogon::HttpRequestPtr& req, const GuardOptions& opts) {
        RequestGuardResult res;
        res.request_id = new_request_id();
        const auto max_resp = config.http.max_json_response_bytes;

        // 1. Hard wire limits before body parsing.
        if (total_header_bytes(req) > config.http.max_header_bytes) {
            res.rejection = make_error_http(
                make_request_error(
                    400, "headers_too_large", "HTTP headers exceed configured limit",
                    {{"too_large", "", "headers exceed limit"}}),
                res.request_id, max_resp);
            return res;
        }
        if (req->query().size() > config.http.max_query_bytes) {
            res.rejection = make_error_http(
                make_request_error(
                    400, "query_too_large", "URL query string exceeds configured limit",
                    {{"too_large", "", "query exceeds limit"}}),
                res.request_id, max_resp);
            return res;
        }
        if (req->body().size() > config.http.max_request_body_bytes) {
            res.rejection = make_error_http(
                make_request_error(
                    413, "payload_too_large", "request body exceeds configured limit",
                    {{"too_large", "", "body exceeds limit"}}),
                res.request_id, max_resp);
            return res;
        }
        if ((req->method() == drogon::Get || req->method() == drogon::Head) &&
            !req->body().empty()) {
            res.rejection = make_error_http(
                make_request_error(
                    400, "unexpected_body", "GET and HEAD requests must not include a body"),
                res.request_id, max_resp);
            return res;
        }
        if (opts.expect_json_body && !content_type_is_json(req->getHeader("content-type"))) {
            res.rejection = make_error_http(
                make_request_error(
                    415, "unsupported_media_type", "Content-Type must be application/json"),
                res.request_id, max_resp);
            return res;
        }

        // 2. Host allowlist check (exact loopback listener match).
        const std::string host = req->getHeader("host");
        if (config.allowed_hosts.find(host) == config.allowed_hosts.end()) {
            log_audit("security_reject_host", res.request_id, "", "invalid_host");
            res.rejection = make_error_http(
                make_request_error(403, "invalid_host", "Host header is not allowed"),
                res.request_id, max_resp);
            return res;
        }

        // 3. API version + authentication.
        const std::string version_hdr = req->getHeader("rodaq-api-version");
        const std::string auth_hdr = req->getHeader("authorization");
        const std::string session_cookie = req->getCookie(kSessionCookieName);

        std::optional<ApiVersion> header_version;
        if (!version_hdr.empty()) {
            header_version = parse_api_version_header(version_hdr);
            if (!header_version) {
                res.rejection = make_error_http(
                    make_request_error(
                        400, "unsupported_api_version", "malformed RoDAQ-API-Version header"),
                    res.request_id, max_resp);
                return res;
            }
            if (header_version->major != kApiMajor) {
                res.rejection = make_error_http(
                    make_request_error(
                        400, "unsupported_api_version", "API major version is not supported"),
                    res.request_id, max_resp);
                return res;
            }
        }

        if (opts.require_auth) {
            if (opts.allow_bearer && !auth_hdr.empty()) {
                if (!header_version) {
                    res.rejection = make_error_http(
                        make_request_error(
                            400, "missing_api_version", "RoDAQ-API-Version header is required"),
                        res.request_id, max_resp);
                    return res;
                }
                const auto bearer = extract_bearer_token(auth_hdr);
                if (!bearer) {
                    log_audit("auth_failure", res.request_id, "", "malformed_bearer");
                    res.rejection = make_error_http(
                        make_request_error(
                            401, "unauthenticated", "invalid Authorization header format"),
                        res.request_id, max_resp);
                    return res;
                }
                auto authed = auth->authenticate_bearer(*bearer, *header_version);
                if (!authed.ok()) {
                    log_audit("auth_failure", res.request_id, "", "bearer_rejected");
                    res.rejection = make_error_http(
                        make_request_error(401, "unauthenticated", "invalid bearer credential"),
                        res.request_id, max_resp);
                    return res;
                }
                res.principal = std::move(authed.value());
            } else if (opts.allow_session && !session_cookie.empty()) {
                auto authed = auth->authenticate_session(session_cookie);
                if (!authed.ok()) {
                    log_audit("auth_failure", res.request_id, "", "session_invalid");
                    res.rejection = make_error_http(
                        make_request_error(
                            401, "unauthenticated", "session missing, expired, or revoked"),
                        res.request_id, max_resp);
                    return res;
                }
                res.principal = std::move(authed.value());
            } else {
                if (!header_version) {
                    res.rejection = make_error_http(
                        make_request_error(
                            400, "missing_api_version", "RoDAQ-API-Version header is required"),
                        res.request_id, max_resp);
                    return res;
                }
                log_audit("auth_failure", res.request_id, "", "no_credentials");
                res.rejection = make_error_http(
                    make_request_error(401, "unauthenticated", "authentication required"),
                    res.request_id, max_resp);
                return res;
            }
        }

        // 4. Authorization scope check.
        if (opts.required_scope && !res.principal.scopes.has(*opts.required_scope)) {
            log_audit(
                "authz_denied", res.request_id, res.principal.principal, "insufficient_scope");
            res.rejection = make_error_http(
                make_request_error(403, "permission_denied", "insufficient principal scope"),
                res.request_id, max_resp, res.principal.api_version);
            return res;
        }

        // 5. Origin + CSRF check for cookie-authenticated mutations.
        if (opts.is_mutation && res.principal.via_session) {
            const std::string origin = req->getHeader("origin");
            if (config.allowed_origins.find(origin) == config.allowed_origins.end()) {
                log_audit(
                    "security_reject_origin", res.request_id, res.principal.principal,
                    "invalid_origin");
                res.rejection = make_error_http(
                    make_request_error(403, "invalid_origin", "Origin header is not allowed"),
                    res.request_id, max_resp, res.principal.api_version);
                return res;
            }
            const std::string csrf = req->getHeader("rodaq-csrf-token");
            if (!constant_time_equal(csrf, res.principal.csrf_token)) {
                log_audit(
                    "security_reject_csrf", res.request_id, res.principal.principal,
                    "invalid_csrf");
                res.rejection = make_error_http(
                    make_request_error(403, "invalid_csrf_token", "missing or invalid CSRF token"),
                    res.request_id, max_resp, res.principal.api_version);
                return res;
            }
        }

        // 6. Server lifecycle phase check.
        const ServerPhase current_phase = phase.load(std::memory_order_acquire);
        if (current_phase == ServerPhase::Recovering && !opts.allow_in_recovering) {
            res.rejection = make_error_http(
                make_request_error(503, "recovering", "server is still recovering Core authority"),
                res.request_id, max_resp, res.principal.api_version);
            return res;
        }
        if (current_phase == ServerPhase::Draining && !opts.allow_in_draining) {
            res.rejection = make_error_http(
                make_request_error(503, "draining", "server is draining for shutdown"),
                res.request_id, max_resp, res.principal.api_version);
            return res;
        }
        if (current_phase == ServerPhase::Failed && !opts.allow_in_recovering) {
            res.rejection = make_error_http(
                make_request_error(503, "server_failed", "server startup failed"), res.request_id,
                max_resp, res.principal.api_version);
            return res;
        }

        res.ok = true;
        return res;
    }

    bool parse_idempotency_and_precondition(
        const drogon::HttpRequestPtr& req, const std::string& request_id, ApiVersion ver,
        RequestIdentity& id_out, ControlPrecondition& pre_out, bool allow_precondition,
        drogon::HttpResponsePtr& error_resp) const {
        const std::string key = req->getHeader("idempotency-key");
        if (!is_visible_ascii(key, 1, 128)) {
            error_resp = make_error_http(
                make_request_error(
                    400, "invalid_idempotency_key",
                    "Idempotency-Key must be 1..128 visible ASCII bytes",
                    {{"invalid_format", "", "invalid Idempotency-Key header"}}),
                request_id, config.http.max_json_response_bytes, ver);
            return false;
        }
        id_out.request_key = key;
        pre_out = {};
        if (allow_precondition) {
            const std::string gen_hdr = req->getHeader("if-control-generation");
            if (!gen_hdr.empty()) {
                const auto parsed = parse_u64_decimal(gen_hdr);
                if (!parsed) {
                    error_resp = make_error_http(
                        make_request_error(
                            400, "invalid_precondition",
                            "If-Control-Generation must be a canonical unsigned decimal string",
                            {{"invalid_format", "", "invalid If-Control-Generation header"}}),
                        request_id, config.http.max_json_response_bytes, ver);
                    return false;
                }
                pre_out.has_expected_control_generation = true;
                pre_out.expected_control_generation = *parsed;
            }
        }
        return true;
    }

    bool parse_page_request(
        const drogon::HttpRequestPtr& req, const std::string& request_id, ApiVersion ver,
        std::uint32_t& limit_out, std::string& cursor_out,
        drogon::HttpResponsePtr& error_resp) const {
        limit_out = std::min<std::uint32_t>(50, config.http.max_page_size);
        cursor_out.clear();
        const std::string limit_param = req->getParameter("limit");
        if (!limit_param.empty()) {
            const auto parsed = parse_u64_decimal(limit_param);
            if (!parsed || *parsed == 0 || *parsed > config.http.max_page_size) {
                error_resp = make_error_http(
                    make_request_error(
                        400, "invalid_query", "limit out of range",
                        {{"out_of_range", "", "limit must be 1..max_page_size"}}),
                    request_id, config.http.max_json_response_bytes, ver);
                return false;
            }
            limit_out = static_cast<std::uint32_t>(*parsed);
        }
        const std::string cursor_param = req->getParameter("cursor");
        if (cursor_param.size() > 256) {
            error_resp = make_error_http(
                make_request_error(
                    400, "invalid_query", "cursor exceeds length limit",
                    {{"too_large", "", "cursor exceeds limit"}}),
                request_id, config.http.max_json_response_bytes, ver);
            return false;
        }
        cursor_out = cursor_param;
        return true;
    }

    void dispatch_core_call(
        CallDomain domain, AdmissionClass admission, const RequestGuardResult& guard,
        std::chrono::nanoseconds timeout,
        std::function<void(const drogon::HttpResponsePtr&)> http_cb,
        std::function<drogon::HttpResponsePtr()> work_fn) {
        auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
            std::move(http_cb));
        const std::string req_id = guard.request_id;
        const ApiVersion ver = guard.principal.api_version;
        const auto max_resp = config.http.max_json_response_bytes;

        auto submitted = executor->submit(
            domain, admission, guard.principal.principal, timeout,
            [shared_cb,
             work_fn = std::move(work_fn)](const std::function<bool()>& try_claim_response) {
                auto resp = work_fn();
                if (try_claim_response()) {
                    (*shared_cb)(resp);
                }
            },
            [shared_cb, req_id, ver, max_resp]() {
                (*shared_cb)(make_error_http(
                    make_request_error(
                        504, "observer_timeout",
                        "observer deadline exceeded; retry with same Idempotency-Key"),
                    req_id, max_resp, ver));
            });

        if (!submitted.admitted) {
            if (submitted.failure == CallAdmissionFailure::PrincipalQuota) {
                (*shared_cb)(make_error_http(
                    make_request_error(
                        429, "rate_limited", "principal request concurrency limit exceeded"),
                    req_id, max_resp, ver));
            } else {
                (*shared_cb)(make_error_http(
                    make_request_error(
                        503, "server_busy", "server execution capacity temporarily saturated"),
                    req_id, max_resp, ver));
            }
        }
    }

    void register_routes() {
        auto& app = drogon::app();

        // POST /api/session
        app.registerHandler(
            "/api/session",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.require_auth = false;
                opts.allow_in_recovering = true;
                opts.allow_in_draining = false;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                const std::string ver_hdr = req->getHeader("rodaq-api-version");
                if (ver_hdr.empty()) {
                    cb(make_error_http(
                        make_request_error(
                            400, "missing_api_version", "RoDAQ-API-Version header is required"),
                        g.request_id, config.http.max_json_response_bytes));
                    return;
                }
                const auto ver = parse_api_version_header(ver_hdr);
                if (!ver || ver->major != kApiMajor) {
                    cb(make_error_http(
                        make_request_error(
                            400, "unsupported_api_version", "API version not supported"),
                        g.request_id, config.http.max_json_response_bytes));
                    return;
                }
                const auto bearer = extract_bearer_token(req->getHeader("authorization"));
                if (!bearer) {
                    log_audit("login_failure", g.request_id, "", "missing_bearer");
                    cb(make_error_http(
                        make_request_error(
                            401, "unauthenticated", "Authorization Bearer token required"),
                        g.request_id, config.http.max_json_response_bytes, *ver));
                    return;
                }
                auto created = auth->create_session(*bearer, *ver);
                if (!created.ok()) {
                    const int status =
                        created.status().code() == StatusCode::ResourceExhausted ? 429 : 401;
                    log_audit(
                        "login_failure", g.request_id, "",
                        status == 429 ? "session_capacity" : "invalid_credential");
                    cb(make_error_http(
                        make_request_error(
                            status,
                            status == 429 ? "session_capacity_exhausted" : "unauthenticated",
                            std::string(created.status().message())),
                        g.request_id, config.http.max_json_response_bytes, *ver));
                    return;
                }
                log_audit("login_success", g.request_id, created.value().principal);
                auto resp = make_json_response(
                    200,
                    make_success_envelope(g.request_id, serialize_session(created.value()), *ver),
                    g.request_id, config.http.max_json_response_bytes, *ver);
                drogon::Cookie cookie(kSessionCookieName, created.value().session_id);
                cookie.setPath("/");
                cookie.setHttpOnly(true);
                cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
                resp->addCookie(std::move(cookie));
                cb(resp);
            },
            {drogon::Post});

        // GET /api/session
        app.registerHandler(
            "/api/session",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.allow_bearer = false;
                opts.allow_session = true;
                opts.allow_in_recovering = true;
                opts.allow_in_draining = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                SessionSnapshot snap;
                snap.session_id = g.principal.session_id;
                snap.principal = g.principal.principal;
                snap.scopes = g.principal.scopes;
                snap.api_version = g.principal.api_version;
                snap.csrf_token = g.principal.csrf_token;
                cb(make_json_response(
                    200,
                    make_success_envelope(g.request_id, serialize_session(snap), snap.api_version),
                    g.request_id, config.http.max_json_response_bytes, snap.api_version));
            },
            {drogon::Get});

        // DELETE /api/session
        app.registerHandler(
            "/api/session",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.is_mutation = true;
                opts.allow_in_recovering = true;
                opts.allow_in_draining = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                if (g.principal.via_session) {
                    auth->destroy_session(g.principal.session_id);
                }
                log_audit("logout", g.request_id, g.principal.principal);
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k204NoContent);
                resp->setContentTypeCode(drogon::CT_NONE);
                resp->addHeader("Cache-Control", "no-store");
                if (req->getHeader("connection") == "close") {
                    resp->setCloseConnection(true);
                }
                apply_common_headers(resp, g.request_id, g.principal.api_version);
                drogon::Cookie clear(kSessionCookieName, "");
                clear.setPath("/");
                clear.setHttpOnly(true);
                clear.setSameSite(drogon::Cookie::SameSite::kStrict);
                clear.setMaxAge(0);
                resp->addCookie(std::move(clear));
                cb(resp);
            },
            {drogon::Delete});

        // GET /api/system (uses QueryReserve so health never starves)
        app.registerHandler(
            "/api/system",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                opts.allow_in_recovering = true;
                opts.allow_in_draining = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Reserve, g, config.timeouts.health,
                    std::move(cb), [this, req_id = g.request_id, ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        SystemSnapshot snap;
                        snap.core_ready = false;
                        snap.recovery.complete = false;
                        snap.recovery.reason = std::string("recovering");
                        snap.captured_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now());
                        if (svc.system) {
                            auto s = svc.system->snapshot();
                            if (s.ok()) {
                                snap = std::move(s.value());
                            }
                        }
                        auto data = serialize_system(
                            phase.load(std::memory_order_acquire), snap, downloads->active_count(),
                            static_assets.deployment_error());
                        return make_json_response(
                            200, make_success_envelope(req_id, std::move(data), ver), req_id,
                            config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // GET /api/capabilities
        app.registerHandler(
            "/api/capabilities",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb), [this, req_id = g.request_id, ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        if (!svc.system) {
                            return make_error_http(
                                make_request_error(503, "recovering", "Core system not ready"),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        auto caps = svc.system->capabilities();
                        auto snap = svc.system->snapshot();
                        if (!caps.ok()) {
                            return make_error_http(
                                map_core_status(caps.status(), FailureDomain::Process), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        SystemSnapshot sys_snap =
                            snap.ok() ? std::move(snap.value()) : SystemSnapshot{};
                        auto data = serialize_capabilities(caps.value(), sys_snap, config);
                        return make_json_response(
                            200, make_success_envelope(req_id, std::move(data), ver), req_id,
                            config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // GET /api/tasks
        app.registerHandler(
            "/api/tasks",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                PageRequest page_req;
                drogon::HttpResponsePtr err_resp;
                if (!parse_page_request(
                        req, g.request_id, g.principal.api_version, page_req.limit, page_req.cursor,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb),
                    [this, page_req = std::move(page_req), req_id = g.request_id,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        if (!svc.tasks) {
                            return make_error_http(
                                make_request_error(503, "recovering", "TaskService unavailable"),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        auto listed = svc.tasks->list_tasks(page_req);
                        if (!listed.ok()) {
                            return make_error_http(
                                map_core_status(listed.status(), FailureDomain::TaskControl),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        return make_json_response(
                            200,
                            make_success_envelope(req_id, serialize_task_page(listed.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // POST /api/tasks
        app.registerHandler(
            "/api/tasks",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.required_scope = Scope::Control;
                opts.is_mutation = true;
                opts.expect_json_body = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                RequestIdentity req_id_obj;
                ControlPrecondition ignored_pre;
                drogon::HttpResponsePtr err_resp;
                if (!parse_idempotency_and_precondition(
                        req, g.request_id, g.principal.api_version, req_id_obj, ignored_pre, false,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                // Strict JSON check before handing raw Portable TaskConfig to Core parser.
                const std::string body(req->body());
                const auto strict = parse_strict_json(body, json_limits());
                if (!strict.ok) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_json", strict.error.message,
                            {{strict.error.code, strict.error.path, strict.error.message}}),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                auto parsed_cfg = parse_task_config(body);
                if (!parsed_cfg.ok()) {
                    cb(make_error_http(
                        map_core_status(parsed_cfg.status(), FailureDomain::Request), g.request_id,
                        config.http.max_json_response_bytes, g.principal.api_version));
                    return;
                }
                dispatch_core_call(
                    CallDomain::Control, AdmissionClass::Ordinary, g,
                    config.timeouts.mutation_acceptance, std::move(cb),
                    [this, cfg = std::move(parsed_cfg.value()), req_id_obj = std::move(req_id_obj),
                     req_id = g.request_id, principal = g.principal.principal,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        if (!svc.tasks) {
                            return make_error_http(
                                make_request_error(503, "recovering", "TaskService unavailable"),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        auto created = svc.tasks->create_task(cfg, req_id_obj);
                        if (!created.ok()) {
                            log_audit("task_create_failed", req_id, principal);
                            return make_error_http(
                                map_core_status(created.status(), FailureDomain::TaskControl),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        std::string config_version;
                        if (auto snap = svc.tasks->get_task(created.value()); snap.ok()) {
                            config_version = snap.value().active_config_version;
                        }
                        log_audit("task_created", req_id, principal, to_string(created.value()));
                        return make_json_response(
                            201,
                            make_success_envelope(
                                req_id, serialize_task_create(created.value(), config_version),
                                ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Post});

        // GET /api/tasks/{task_id}
        app.registerHandler(
            "/api/tasks/{1}",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_id = parse_task_id(raw_task_id);
                if (!parsed_id.ok()) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_task_id", "malformed task_id",
                            {{"invalid_format", "", "invalid task_id"}}),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb),
                    [this, tid = parsed_id.value(), req_id = g.request_id,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto snap = svc.tasks->get_task(tid);
                        if (!snap.ok()) {
                            return make_error_http(
                                map_core_status(snap.status(), FailureDomain::TaskControl), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        return make_json_response(
                            200,
                            make_success_envelope(req_id, serialize_task_data(snap.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // GET /api/tasks/{task_id}/config (raw Portable TaskConfig without API envelope!)
        app.registerHandler(
            "/api/tasks/{1}/config",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_id = parse_task_id(raw_task_id);
                if (!parsed_id.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb),
                    [this, tid = parsed_id.value(), req_id = g.request_id,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto cfg = svc.tasks->get_active_config(tid);
                        if (!cfg.ok()) {
                            return make_error_http(
                                map_core_status(cfg.status(), FailureDomain::TaskControl), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        auto exported = export_task_config(cfg.value());
                        if (!exported.ok()) {
                            return make_error_http(
                                map_core_status(exported.status(), FailureDomain::TaskControl),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        auto snap = svc.tasks->get_task(tid);
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setContentTypeCodeAndCustomString(
                            drogon::CT_CUSTOM, "application/json; charset=utf-8");
                        resp->setBody(std::move(exported.value()));
                        resp->addHeader("Cache-Control", "no-store");
                        if (snap.ok() && !snap.value().active_config_version.empty()) {
                            resp->addHeader(
                                "ETag", "\"" + snap.value().active_config_version + "\"");
                        }
                        apply_common_headers(resp, req_id, ver);
                        return resp;
                    });
            },
            {drogon::Get});

        // PUT /api/tasks/{task_id}/config
        app.registerHandler(
            "/api/tasks/{1}/config",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Control;
                opts.is_mutation = true;
                opts.expect_json_body = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_id = parse_task_id(raw_task_id);
                if (!parsed_id.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                RequestIdentity req_id_obj;
                ControlPrecondition precondition;
                drogon::HttpResponsePtr err_resp;
                if (!parse_idempotency_and_precondition(
                        req, g.request_id, g.principal.api_version, req_id_obj, precondition, true,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                const std::string body(req->body());
                const auto strict = parse_strict_json(body, json_limits());
                if (!strict.ok) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_json", strict.error.message,
                            {{strict.error.code, strict.error.path, strict.error.message}}),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                auto parsed_cfg = parse_task_config(body);
                if (!parsed_cfg.ok()) {
                    cb(make_error_http(
                        map_core_status(parsed_cfg.status(), FailureDomain::Request), g.request_id,
                        config.http.max_json_response_bytes, g.principal.api_version));
                    return;
                }
                dispatch_core_call(
                    CallDomain::Control, AdmissionClass::Ordinary, g,
                    config.timeouts.mutation_acceptance, std::move(cb),
                    [this, tid = parsed_id.value(), cfg = std::move(parsed_cfg.value()),
                     req_id_obj = std::move(req_id_obj), precondition, req_id = g.request_id,
                     principal = g.principal.principal, ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto op = svc.tasks->configure(tid, cfg, req_id_obj, precondition);
                        if (!op.ok()) {
                            log_audit("task_configure_failed", req_id, principal, to_string(tid));
                            return make_error_http(
                                map_core_status(op.status(), FailureDomain::TaskControl), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        log_audit(
                            "task_configure_accepted", req_id, principal, to_string(op.value()));
                        return make_json_response(
                            202,
                            make_success_envelope(
                                req_id, serialize_mutation_accepted(tid, op.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Put});

        // POST /api/tasks/{task_id}/operations
        app.registerHandler(
            "/api/tasks/{1}/operations",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Control;
                opts.is_mutation = true;
                opts.expect_json_body = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_id = parse_task_id(raw_task_id);
                if (!parsed_id.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                RequestIdentity req_id_obj;
                ControlPrecondition precondition;
                drogon::HttpResponsePtr err_resp;
                if (!parse_idempotency_and_precondition(
                        req, g.request_id, g.principal.api_version, req_id_obj, precondition, true,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                ApiError dto_error;
                auto dto = parse_operation_request(req->body(), json_limits(), dto_error);
                if (!dto.ok()) {
                    cb(make_error_http(
                        dto_error, g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                const auto kind = dto.value().kind;
                // Reset and Retire require Admin scope on EVERY attempt including retries.
                if ((kind == OperationKind::Reset || kind == OperationKind::Retire) &&
                    !g.principal.scopes.has(Scope::Admin)) {
                    log_audit(
                        "authz_denied_admin", g.request_id, g.principal.principal,
                        to_wire_string(kind));
                    cb(make_error_http(
                        make_request_error(
                            403, "permission_denied", "admin scope required for reset or retire"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                // Stop uses ControlReserve so hung ordinary mutations never starve Stop.
                const AdmissionClass admission = (kind == OperationKind::Stop)
                                                     ? AdmissionClass::Reserve
                                                     : AdmissionClass::Ordinary;
                dispatch_core_call(
                    CallDomain::Control, admission, g, config.timeouts.mutation_acceptance,
                    std::move(cb),
                    [this, tid = parsed_id.value(), kind, req_id_obj = std::move(req_id_obj),
                     precondition, req_id = g.request_id, principal = g.principal.principal,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        Result<OperationId> op(Status(StatusCode::Internal, "unhandled kind"));
                        switch (kind) {
                            case OperationKind::Start:
                                op = svc.tasks->start(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Pause:
                                op = svc.tasks->pause(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Resume:
                                op = svc.tasks->resume(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Stop:
                                op = svc.tasks->stop(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Reset:
                                op = svc.tasks->reset(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Retire:
                                op = svc.tasks->retire(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Configure:
                                break;
                        }
                        if (!op.ok()) {
                            log_audit(
                                "task_op_failed", req_id, principal,
                                std::string(to_wire_string(kind)) + ":" + to_string(tid));
                            return make_error_http(
                                map_core_status(op.status(), FailureDomain::TaskControl), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        log_audit(
                            "task_op_accepted", req_id, principal,
                            std::string(to_wire_string(kind)) + ":" + to_string(op.value()));
                        return make_json_response(
                            202,
                            make_success_envelope(
                                req_id, serialize_mutation_accepted(tid, op.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Post});

        // GET /api/tasks/{task_id}/operations
        app.registerHandler(
            "/api/tasks/{1}/operations",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_id = parse_task_id(raw_task_id);
                if (!parsed_id.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                PageRequest page_req;
                drogon::HttpResponsePtr err_resp;
                if (!parse_page_request(
                        req, g.request_id, g.principal.api_version, page_req.limit, page_req.cursor,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb),
                    [this, tid = parsed_id.value(), page_req = std::move(page_req),
                     req_id = g.request_id, ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto page = svc.tasks->list_operations(tid, page_req);
                        if (!page.ok()) {
                            return make_error_http(
                                map_core_status(page.status(), FailureDomain::TaskControl), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        return make_json_response(
                            200,
                            make_success_envelope(
                                req_id, serialize_operation_page(tid, page.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // GET /api/tasks/{task_id}/operations/{operation_id} (uses QueryReserve!)
        app.registerHandler(
            "/api/tasks/{1}/operations/{2}",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id, const std::string& raw_op_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                opts.allow_in_draining = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_tid = parse_task_id(raw_task_id);
                auto parsed_oid = parse_operation_id(raw_op_id);
                if (!parsed_tid.ok() || !parsed_oid.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_id", "malformed task_id or operation_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Reserve, g, config.timeouts.query,
                    std::move(cb),
                    [this, tid = parsed_tid.value(), oid = parsed_oid.value(),
                     req_id = g.request_id, ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto op = svc.tasks->get_operation(tid, oid);
                        if (!op.ok()) {
                            return make_error_http(
                                map_core_status(op.status(), FailureDomain::TaskControl, true),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        return make_json_response(
                            200,
                            make_success_envelope(
                                req_id, serialize_operation(tid, op.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // GET /api/tasks/{task_id}/runtime
        app.registerHandler(
            "/api/tasks/{1}/runtime",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_tid = parse_task_id(raw_task_id);
                if (!parsed_tid.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb),
                    [this, tid = parsed_tid.value(), req_id = g.request_id,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto rt = svc.tasks->get_runtime_snapshot(tid);
                        if (!rt.ok()) {
                            return make_error_http(
                                map_core_status(rt.status(), FailureDomain::Pipeline), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        return make_json_response(
                            200,
                            make_success_envelope(req_id, serialize_task_runtime(rt.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // GET /api/artifacts
        app.registerHandler(
            "/api/artifacts",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                ArtifactQuery query;
                drogon::HttpResponsePtr err_resp;
                if (!parse_page_request(
                        req, g.request_id, g.principal.api_version, query.limit, query.cursor,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                const auto invalid_filter = [&](std::string msg) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_query", std::move(msg), {{"invalid_format", "", ""}}),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                };
                if (const auto s = req->getParameter("task_id"); !s.empty()) {
                    auto tid = parse_task_id(s);
                    if (!tid.ok()) {
                        invalid_filter("invalid task_id filter");
                        return;
                    }
                    query.task_id = tid.value();
                }
                if (const auto s = req->getParameter("pipeline_id"); !s.empty()) {
                    auto pid = parse_u64_decimal(s);
                    if (!pid || *pid == 0) {
                        invalid_filter("invalid pipeline_id filter");
                        return;
                    }
                    query.pipeline_id = *pid;
                }
                if (const auto s = req->getParameter("format"); !s.empty()) {
                    if (!is_visible_ascii(s, 1, 128)) {
                        invalid_filter("invalid format filter");
                        return;
                    }
                    query.format = s;
                }
                const auto rec_begin = req->getParameter("recording_begin");
                const auto rec_end = req->getParameter("recording_end");
                if (!rec_begin.empty() || !rec_end.empty()) {
                    if (!query.task_id) {
                        invalid_filter("recording_begin/recording_end require task_id");
                        return;
                    }
                    auto b =
                        rec_begin.empty()
                            ? std::optional<std::chrono::nanoseconds>(std::chrono::nanoseconds{0})
                            : parse_duration_query(rec_begin);
                    auto e = rec_end.empty()
                                 ? std::optional<std::chrono::nanoseconds>(std::chrono::nanoseconds{
                                       std::numeric_limits<std::int64_t>::max()})
                                 : parse_duration_query(rec_end);
                    if (!b || !e || *b > *e) {
                        invalid_filter("invalid recording_begin/recording_end duration range");
                        return;
                    }
                    query.recording_range = RecordingRange{
                        static_cast<std::uint64_t>(b->count()),
                        static_cast<std::uint64_t>(e->count())};
                }
                const auto rw_begin = req->getParameter("receive_wall_begin");
                const auto rw_end = req->getParameter("receive_wall_end");
                if (!rw_begin.empty() || !rw_end.empty()) {
                    auto b = rw_begin.empty() ? std::optional<WallTime>(WallTime{})
                                              : parse_rfc3339_utc(rw_begin);
                    auto e = rw_end.empty() ? std::optional<WallTime>(WallTime::max())
                                            : parse_rfc3339_utc(rw_end);
                    if (!b || !e || *b > *e) {
                        invalid_filter("invalid receive_wall_begin/receive_wall_end");
                        return;
                    }
                    query.receive_wall_time_range = ArtifactWallRange{*b, *e};
                }
                const auto cm_begin = req->getParameter("committed_begin");
                const auto cm_end = req->getParameter("committed_end");
                if (!cm_begin.empty() || !cm_end.empty()) {
                    auto b = cm_begin.empty() ? std::optional<WallTime>(WallTime{})
                                              : parse_rfc3339_utc(cm_begin);
                    auto e = cm_end.empty() ? std::optional<WallTime>(WallTime::max())
                                            : parse_rfc3339_utc(cm_end);
                    if (!b || !e || *b > *e) {
                        invalid_filter("invalid committed_begin/committed_end");
                        return;
                    }
                    query.committed_at = ArtifactWallRange{*b, *e};
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb),
                    [this, query = std::move(query), req_id = g.request_id,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto page = svc.artifacts->query_artifacts(query);
                        if (!page.ok()) {
                            return make_error_http(
                                map_core_status(page.status(), FailureDomain::Storage), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        return make_json_response(
                            200,
                            make_success_envelope(
                                req_id, serialize_artifact_page(page.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // GET /api/artifacts/{artifact_id}
        app.registerHandler(
            "/api/artifacts/{1}",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_art_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto aid = parse_artifact_id(raw_art_id);
                if (!aid.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_artifact_id", "malformed artifact_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                dispatch_core_call(
                    CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                    std::move(cb),
                    [this, id = aid.value(), req_id = g.request_id,
                     ver = g.principal.api_version]() {
                        const auto svc = current_services();
                        auto info = svc.artifacts->get_artifact(id);
                        if (!info.ok()) {
                            return make_error_http(
                                map_core_status(info.status(), FailureDomain::Storage), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        return make_json_response(
                            200,
                            make_success_envelope(
                                req_id, serialize_artifact_detail(info.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Get});

        // HEAD / GET /api/artifacts/{artifact_id}/files/{file_id} (ArtifactReadHandle ->
        // DownloadSession)
        app.registerHandler(
            "/api/artifacts/{1}/files/{2}",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_art_id, const std::string& raw_file_id) {
                GuardOptions opts;
                opts.required_scope = Scope::Read;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto aid = parse_artifact_id(raw_art_id);
                auto fid = parse_artifact_file_id(raw_file_id);
                if (!aid.ok() || !fid.ok()) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_artifact_file_id", "malformed artifact_id or file_id"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                    return;
                }
                if (req->isHead()) {
                    const bool close_conn = (req->getHeader("connection") == "close");
                    dispatch_core_call(
                        CallDomain::Query, AdmissionClass::Ordinary, g, config.timeouts.query,
                        std::move(cb),
                        [this, art_id = aid.value(), file_id = fid.value(), req_id = g.request_id,
                         ver = g.principal.api_version, close_conn]() {
                            const auto svc = current_services();
                            auto info = svc.artifacts->get_artifact(art_id);
                            if (!info.ok()) {
                                return make_error_http(
                                    map_core_status(info.status(), FailureDomain::Storage), req_id,
                                    config.http.max_json_response_bytes, ver);
                            }
                            const ArtifactFileInfo* matched = nullptr;
                            for (const auto& f : info.value().files) {
                                if (f.file_id == file_id) {
                                    matched = &f;
                                    break;
                                }
                            }
                            if (!matched) {
                                return make_error_http(
                                    make_request_error(404, "not_found", "artifact file not found"),
                                    req_id, config.http.max_json_response_bytes, ver);
                            }
                            auto resp = drogon::HttpResponse::newHttpResponse();
                            resp->setStatusCode(drogon::k200OK);
                            resp->setPassThrough(true);
                            resp->addHeader("Content-Type", matched->media_type);
                            resp->addHeader("Accept-Ranges", "bytes");
                            resp->addHeader("Content-Length", std::to_string(matched->size));
                            resp->addHeader("ETag", "\"" + matched->etag + "\"");
                            resp->addHeader(
                                "Content-Disposition",
                                build_content_disposition(matched->display_name));
                            resp->addHeader("Cache-Control", "private, no-store");
                            if (close_conn) {
                                resp->setCloseConnection(true);
                                resp->addHeader("Connection", "close");
                            }
                            apply_common_headers(resp, req_id, ver);
                            return resp;
                        });
                    return;
                }
                const std::string range_hdr = req->getHeader("range");
                const std::string if_range_hdr = req->getHeader("if-range");
                const auto conn_weak = req->getConnectionPtr();
                auto shared_cb =
                    std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                        std::move(cb));

                // Acquire ArtifactReadHandle off the event loop (filesystem open/lease).
                auto submitted = executor->submit(
                    CallDomain::Query, AdmissionClass::Ordinary, g.principal.principal,
                    config.timeouts.query,
                    [this, art_id = aid.value(), file_id = fid.value(), range_hdr, if_range_hdr,
                     conn_weak, shared_cb, req_id = g.request_id, principal = g.principal.principal,
                     ver = g.principal.api_version](const std::function<bool()>& try_claim) {
                        const auto svc = current_services();
                        auto info = svc.artifacts->get_artifact(art_id);
                        if (!info.ok()) {
                            if (try_claim()) {
                                (*shared_cb)(make_error_http(
                                    map_core_status(info.status(), FailureDomain::Storage), req_id,
                                    config.http.max_json_response_bytes, ver));
                            }
                            return;
                        }
                        const ArtifactFileInfo* matched = nullptr;
                        for (const auto& f : info.value().files) {
                            if (f.file_id == file_id) {
                                matched = &f;
                                break;
                            }
                        }
                        if (!matched) {
                            if (try_claim()) {
                                (*shared_cb)(make_error_http(
                                    make_request_error(404, "not_found", "artifact file not found"),
                                    req_id, config.http.max_json_response_bytes, ver));
                            }
                            return;
                        }
                        auto acquired = svc.artifacts->acquire_read(art_id, file_id);
                        if (!acquired.ok()) {
                            if (try_claim()) {
                                (*shared_cb)(make_error_http(
                                    map_core_status(acquired.status(), FailureDomain::Storage),
                                    req_id, config.http.max_json_response_bytes, ver));
                            }
                            return;
                        }
                        auto handle =
                            std::make_shared<ArtifactReadHandle>(std::move(acquired.value()));
                        const std::uint64_t full_length = handle->size();

                        std::string effective_range = range_hdr;
                        if (!effective_range.empty() && !if_range_hdr.empty() &&
                            !if_range_matches_etag(if_range_hdr, matched->etag)) {
                            effective_range.clear();
                        }
                        const auto parsed_range =
                            parse_single_byte_range(effective_range, full_length);
                        if (parsed_range.status == RangeParseStatus::MultipleRanges) {
                            if (try_claim()) {
                                (*shared_cb)(make_error_http(
                                    make_request_error(
                                        400, "multiple_ranges_not_supported",
                                        "V1 supports at most one byte-range-spec"),
                                    req_id, config.http.max_json_response_bytes, ver));
                            }
                            return;
                        }
                        if (parsed_range.status == RangeParseStatus::Malformed) {
                            if (try_claim()) {
                                (*shared_cb)(make_error_http(
                                    make_request_error(
                                        400, "invalid_range", "malformed Range header"),
                                    req_id, config.http.max_json_response_bytes, ver));
                            }
                            return;
                        }
                        if (parsed_range.status == RangeParseStatus::Unsatisfiable) {
                            if (try_claim()) {
                                auto resp = make_error_http(
                                    make_request_error(
                                        416, "range_not_satisfiable",
                                        "requested Range unsatisfiable"),
                                    req_id, config.http.max_json_response_bytes, ver);
                                resp->addHeader(
                                    "Content-Range", "bytes */" + std::to_string(full_length));
                                (*shared_cb)(resp);
                            }
                            return;
                        }

                        if (!try_claim()) {
                            return;
                        }
                        auto conn = conn_weak.lock();
                        if (!conn || conn->disconnected()) {
                            return;
                        }
                        auto admitted = downloads->create_session(
                            principal, parsed_range.range, std::move(handle), conn_weak);
                        if (!admitted.admitted) {
                            const int status =
                                admitted.failure == DownloadAdmissionFailure::PrincipalQuota ? 429
                                                                                             : 503;
                            (*shared_cb)(make_error_http(
                                make_request_error(
                                    status,
                                    status == 429 ? "download_quota_exceeded" : "downloads_busy",
                                    "download concurrency limit reached"),
                                req_id, config.http.max_json_response_bytes, ver));
                            return;
                        }

                        log_audit(
                            "download_started", req_id, principal,
                            to_string(art_id) + "/" + to_string(file_id));

                        // Serialize HTTP/1.1 status + headers directly into Trantor AsyncStream
                        // with exact Content-Length and Connection: close so Drogon neither forces
                        // chunked encoding nor shuts down the socket before body chunks drain.
                        std::string header_block;
                        header_block.reserve(512);
                        header_block += parsed_range.range.partial
                                            ? "HTTP/1.1 206 Partial Content\r\n"
                                            : "HTTP/1.1 200 OK\r\n";
                        header_block += "Content-Type: " + matched->media_type + "\r\n";
                        header_block += "Accept-Ranges: bytes\r\n";
                        header_block +=
                            "Content-Length: " + std::to_string(parsed_range.range.length) + "\r\n";
                        header_block += "ETag: \"" + matched->etag + "\"\r\n";
                        header_block += "Content-Disposition: " +
                                        build_content_disposition(matched->display_name) + "\r\n";
                        header_block += "Cache-Control: private, no-store\r\n";
                        if (parsed_range.range.partial) {
                            header_block += "Content-Range: bytes " +
                                            std::to_string(parsed_range.range.start) + "-" +
                                            std::to_string(parsed_range.range.end) + "/" +
                                            std::to_string(full_length) + "\r\n";
                        }
                        header_block += "RoDAQ-API-Version: " + std::to_string(ver.major) + "." +
                                        std::to_string(ver.minor) + "\r\n";
                        header_block += "RoDAQ-Request-ID: " + req_id + "\r\n";
                        header_block +=
                            std::string("Content-Security-Policy: ") + kCspHeaderValue + "\r\n";
                        header_block += "Referrer-Policy: no-referrer\r\n";
                        header_block += "X-Content-Type-Options: nosniff\r\n";
                        header_block += "X-Frame-Options: DENY\r\n";
                        header_block += "Connection: close\r\n\r\n";

                        auto* loop = conn->getLoop();
                        auto session = std::move(admitted.session);
                        loop->runInLoop([conn, header_block = std::move(header_block),
                                         session = std::move(session)]() mutable {
                            if (conn->disconnected()) {
                                session->request_cancel();
                                return;
                            }
                            auto stream = conn->sendAsyncStream(true);
                            session->attach_and_start(std::move(stream), std::move(header_block));
                        });
                    },
                    [this, shared_cb, req_id = g.request_id, ver = g.principal.api_version]() {
                        (*shared_cb)(make_error_http(
                            make_request_error(504, "observer_timeout", "download setup timed out"),
                            req_id, config.http.max_json_response_bytes, ver));
                    });

                if (!submitted.admitted) {
                    const int status =
                        submitted.failure == CallAdmissionFailure::PrincipalQuota ? 429 : 503;
                    (*shared_cb)(make_error_http(
                        make_request_error(
                            status, status == 429 ? "rate_limited" : "server_busy",
                            "server capacity exhausted"),
                        g.request_id, config.http.max_json_response_bytes,
                        g.principal.api_version));
                }
            },
            {drogon::Get, drogon::Head});

        // Static SPA / 404 handler (never falls back /api/* to index.html!).
        app.setCustom404Page(drogon::HttpResponse::newNotFoundResponse());
        app.registerHandlerViaRegex(
            "^/(.*)$",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                const std::string req_id = new_request_id();
                const std::string path = req->path();
                if (path == "/api" || path.rfind("/api/", 0) == 0) {
                    cb(make_error_http(
                        make_request_error(404, "not_found", "API route not found"), req_id,
                        config.http.max_json_response_bytes));
                    return;
                }
                if (req->method() != drogon::Get && req->method() != drogon::Head) {
                    cb(make_error_http(
                        make_request_error(405, "method_not_allowed", "method not allowed"), req_id,
                        config.http.max_json_response_bytes));
                    return;
                }
                const auto resolved = static_assets.resolve(path);
                if (!resolved.found) {
                    cb(make_error_http(
                        make_request_error(404, "not_found", "static asset not found"), req_id,
                        config.http.max_json_response_bytes));
                    return;
                }
                if (!req->isHead()) {
                    auto resp = drogon::HttpResponse::newFileResponse(
                        resolved.file_path.string(), "", drogon::CT_CUSTOM, resolved.content_type);
                    resp->addHeader("Cache-Control", resolved.cache_control);
                    apply_common_headers(resp, req_id);
                    cb(resp);
                    return;
                }
                std::error_code ec;
                const auto size = std::filesystem::file_size(resolved.file_path, ec);
                if (ec) {
                    cb(make_error_http(
                        make_request_error(404, "not_found", "static asset unreadable"), req_id,
                        config.http.max_json_response_bytes));
                    return;
                }
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setPassThrough(true);
                resp->addHeader("Content-Type", resolved.content_type);
                resp->addHeader("Content-Length", std::to_string(size));
                resp->addHeader("Cache-Control", resolved.cache_control);
                if (req->getHeader("connection") == "close") {
                    resp->setCloseConnection(true);
                }
                apply_common_headers(resp, req_id);
                cb(resp);
            },
            {drogon::Get, drogon::Head, drogon::Post, drogon::Put, drogon::Delete});
    }
};

ServerApp::ServerApp() : impl_(std::make_unique<Impl>()) {}

ServerApp::~ServerApp() noexcept {
    if (impl_ && impl_->started.load(std::memory_order_acquire) &&
        !impl_->stopped.load(std::memory_order_acquire)) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

Result<void> ServerApp::start(
    ServerConfig config, ServerServices services, ServerPhase initial_phase) {
    if (impl_->started.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "ServerApp already started");
    }
    auto valid = validate_server_config(config);
    if (!valid.ok()) {
        return valid.status();
    }
    auto auth = AuthState::create(config.security);
    if (!auth.ok()) {
        return auth.status();
    }
    auto exec = ServiceCallExecutor::create(config.execution);
    if (!exec.ok()) {
        return exec.status();
    }
    auto dl = DownloadRegistry::create(config.download);
    if (!dl.ok()) {
        return dl.status();
    }

    impl_->config = std::move(config);
    {
        std::lock_guard<std::mutex> lock(impl_->services_mutex);
        impl_->services = services;
    }
    impl_->phase.store(initial_phase, std::memory_order_release);
    impl_->auth = std::move(auth.value());
    impl_->executor = std::move(exec.value());
    impl_->downloads = std::move(dl.value());
    impl_->static_assets = StaticAssetService::create(impl_->config.static_assets_root, kApiMajor);

    auto& app = drogon::app();
    app.setThreadNum(impl_->config.http.worker_threads);
    app.setClientMaxBodySize(impl_->config.http.max_request_body_bytes + 4096);
    app.setIdleConnectionTimeout(60);
    app.disableSigtermHandling();
    app.setConnectionCallback(
        [dl_raw = impl_->downloads.get()](const trantor::TcpConnectionPtr& conn) {
            if (conn && conn->disconnected()) {
                dl_raw->notify_connection_closed(conn.get());
            }
        });
    app.addListener(impl_->config.http.listen_address, impl_->config.http.port);
    impl_->register_routes();

    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    bool loop_running = false;
    app.registerBeginningAdvice([&] {
        std::lock_guard<std::mutex> lock(ready_mutex);
        loop_running = true;
        ready_cv.notify_all();
    });

    impl_->started.store(true, std::memory_order_release);
    impl_->drogon_thread = std::thread([] { drogon::app().run(); });

    std::unique_lock<std::mutex> lock(ready_mutex);
    if (!ready_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return loop_running; })) {
        return Status(StatusCode::Unavailable, "Drogon event loop did not start within 5s");
    }
    RODAQ_LOG_INFO(
        "server started listen={}:{} phase={}", impl_->config.http.listen_address,
        impl_->config.http.port, to_wire_string(initial_phase));
    return {};
}

Result<void> ServerApp::mark_ready(ServerServices services) {
    if (!impl_->started.load(std::memory_order_acquire) ||
        impl_->stopped.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "ServerApp is not running");
    }
    if (!services.tasks || !services.system || !services.artifacts) {
        return Status(StatusCode::InvalidArgument, "ServerServices pointers must be non-null");
    }
    {
        std::lock_guard<std::mutex> lock(impl_->services_mutex);
        impl_->services = services;
    }
    ServerPhase expected = ServerPhase::Recovering;
    (void)impl_->phase.compare_exchange_strong(
        expected, ServerPhase::Ready, std::memory_order_acq_rel);
    RODAQ_LOG_INFO("server transitioned phase=ready");
    return {};
}

Result<void> ServerApp::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (!impl_->started.load(std::memory_order_acquire) ||
        impl_->stopped.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }
    impl_->phase.store(ServerPhase::Draining, std::memory_order_release);
    RODAQ_LOG_INFO("server entering phase=draining");

    // 1. Cancel all active downloads immediately and stop accepting new HTTP work.
    if (impl_->downloads) {
        impl_->downloads->cancel_all();
    }

    // 2. Drain downloads and in-flight Core calls within bounded deadline.
    Result<void> dl_res;
    if (impl_->downloads) {
        dl_res = impl_->downloads->shutdown(deadline);
    }
    Result<void> exec_res;
    if (impl_->executor) {
        exec_res = impl_->executor->shutdown(deadline);
    }

    // 3. Stop Drogon event loops cleanly.
    drogon::app().quit();
    if (impl_->drogon_thread.joinable()) {
        impl_->drogon_thread.join();
    }

    RODAQ_LOG_INFO("server shutdown complete");
    if (!dl_res.ok()) {
        return dl_res.status();
    }
    return exec_res;
}

ServerPhase ServerApp::phase() const noexcept {
    return impl_->phase.load(std::memory_order_acquire);
}

ExecutorMetrics ServerApp::executor_metrics() const {
    return impl_->executor ? impl_->executor->metrics() : ExecutorMetrics{};
}

DownloadMetrics ServerApp::download_metrics() const {
    return impl_->downloads ? impl_->downloads->metrics() : DownloadMetrics{};
}

std::size_t ServerApp::active_sessions() const {
    return impl_->auth ? impl_->auth->active_session_count() : 0;
}

}  // namespace rodaq::server
