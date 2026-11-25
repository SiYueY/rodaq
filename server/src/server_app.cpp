#include "server_app.hpp"

#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <rodaq/logging.hpp>
#include <rodaq/task/config.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <utility>

namespace rodaq::server {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::uint32_t kSupportedApiMajor = 1;
constexpr std::uint32_t kSupportedApiMinor = 0;
constexpr std::string_view kSessionCookieName = "rodaq_session";

std::string next_request_id() {
    auto tok = generate_secure_token();
    if (tok.ok()) {
        return tok.value().substr(0, 32);
    }
    static std::atomic<std::uint64_t> fallback{1};
    return "req-" + std::to_string(fallback.fetch_add(1, std::memory_order_relaxed));
}

bool host_allowed(const ServerConfig& cfg, std::string_view host_header) noexcept {
    if (host_header.empty()) {
        return false;
    }
    std::string lower(host_header);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::find(cfg.allowed_hosts.begin(), cfg.allowed_hosts.end(), lower) !=
           cfg.allowed_hosts.end();
}

bool origin_allowed(const ServerConfig& cfg, std::string_view origin_header) noexcept {
    if (origin_header.empty()) {
        return false;
    }
    std::string lower(origin_header);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::find(cfg.allowed_origins.begin(), cfg.allowed_origins.end(), lower) !=
           cfg.allowed_origins.end();
}

bool is_json_content_type(std::string_view ct) noexcept {
    const auto semi = ct.find(';');
    std::string media(semi == std::string_view::npos ? ct : ct.substr(0, semi));
    while (!media.empty() && std::isspace(static_cast<unsigned char>(media.back()))) {
        media.pop_back();
    }
    while (!media.empty() && std::isspace(static_cast<unsigned char>(media.front()))) {
        media.erase(media.begin());
    }
    std::transform(media.begin(), media.end(), media.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return media == "application/json";
}

std::optional<std::string> extract_bearer(const drogon::HttpRequestPtr& req) {
    const auto auth = req->getHeader("authorization");
    constexpr std::string_view kPrefix = "Bearer ";
    if (auth.size() <= kPrefix.size() || auth.compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    return auth.substr(kPrefix.size());
}

std::string mime_type_for_extension(const fs::path& path) {
    const auto ext = path.extension().string();
    if (ext == ".html") {
        return "text/html; charset=utf-8";
    }
    if (ext == ".js" || ext == ".mjs") {
        return "application/javascript; charset=utf-8";
    }
    if (ext == ".css") {
        return "text/css; charset=utf-8";
    }
    if (ext == ".svg") {
        return "image/svg+xml";
    }
    if (ext == ".json") {
        return "application/json; charset=utf-8";
    }
    if (ext == ".png") {
        return "image/png";
    }
    if (ext == ".ico") {
        return "image/x-icon";
    }
    if (ext == ".woff2") {
        return "font/woff2";
    }
    return "application/octet-stream";
}

struct PipelineAuthResult {
    bool ok{false};
    std::string request_id;
    ApiVersion version{kSupportedApiMajor, kSupportedApiMinor};
    AuthContext auth;
    drogon::HttpResponsePtr error_response;
};

PipelineAuthResult run_ingress_pipeline(
    const drogon::HttpRequestPtr& req, const ServerConfig& cfg, AuthState& auth_state,
    ServerPhase phase, bool allow_during_recovering, Scope required_scope, bool is_mutation,
    bool require_json_body, bool allow_cookie_without_version_header = false) {
    PipelineAuthResult out;
    out.request_id = next_request_id();

    // 1. Hard query & body size limits before parsing.
    if (req->query().size() > cfg.http.max_query_bytes) {
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k400BadRequest,
             "query_too_large",
             "Query string exceeds server limit",
             "never",
             "request",
             "none",
             {}});
        return out;
    }
    if (req->body().size() > cfg.http.max_request_body_bytes) {
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k413RequestEntityTooLarge,
             "payload_too_large",
             "Request body exceeds server limit",
             "never",
             "request",
             "none",
             {}});
        return out;
    }

    // 2. Host allowlist check.
    if (!host_allowed(cfg, req->getHeader("host"))) {
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k400BadRequest,
             "invalid_host",
             "Host header is not in loopback allowlist",
             "never",
             "request",
             "none",
             {}});
        return out;
    }

    // 3. Authentication (Bearer or Session Cookie).
    const auto bearer = extract_bearer(req);
    const auto cookie_sid = req->getCookie(std::string(kSessionCookieName));
    const auto ver_hdr = req->getHeader("rodaq-api-version");

    if (bearer) {
        if (ver_hdr.empty()) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k400BadRequest,
                 "missing_api_version",
                 "RoDAQ-API-Version header is required",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
        auto parsed_v = parse_api_version_header(ver_hdr);
        if (!parsed_v.ok() || parsed_v.value().major != kSupportedApiMajor) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k400BadRequest,
                 "unsupported_api_version",
                 "Unsupported RoDAQ-API-Version",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
        out.version = parsed_v.value();
        auto authed = auth_state.authenticate_bearer(*bearer, out.version);
        if (!authed.ok()) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k401Unauthorized,
                 "unauthenticated",
                 "Authentication required",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
        out.auth = std::move(authed.value());
    } else if (!cookie_sid.empty()) {
        auto authed = auth_state.authenticate_session(cookie_sid);
        if (!authed.ok()) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k401Unauthorized,
                 "unauthenticated",
                 "Session invalid or expired",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
        out.auth = std::move(authed.value());
        out.version = out.auth.api_version;
        if (!ver_hdr.empty()) {
            auto parsed_v = parse_api_version_header(ver_hdr);
            if (!parsed_v.ok() || parsed_v.value().major != out.auth.api_version.major) {
                out.error_response = make_error_response(
                    out.version, out.request_id,
                    {drogon::k400BadRequest,
                     "unsupported_api_version",
                     "API version does not match negotiated session version",
                     "never",
                     "request",
                     "none",
                     {}});
                return out;
            }
        } else if (!allow_cookie_without_version_header) {
            // Browser navigation/download endpoints allow omitting header; Fetch endpoints may pass
            // or rely on negotiated session version.
        }
    } else {
        if (ver_hdr.empty()) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k400BadRequest,
                 "missing_api_version",
                 "RoDAQ-API-Version header is required",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
        auto parsed_v = parse_api_version_header(ver_hdr);
        if (!parsed_v.ok() || parsed_v.value().major != kSupportedApiMajor) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k400BadRequest,
                 "unsupported_api_version",
                 "Unsupported RoDAQ-API-Version",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k401Unauthorized,
             "unauthenticated",
             "Authentication required",
             "never",
             "request",
             "none",
             {}});
        return out;
    }

    // 4. Server phase check (Recovering limited-mode & Draining).
    if (phase == ServerPhase::Draining && is_mutation) {
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k503ServiceUnavailable,
             "draining",
             "Server is shutting down",
             "after_process_restart",
             "process",
             "none",
             {}});
        return out;
    }
    if (phase == ServerPhase::Recovering && !allow_during_recovering) {
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k503ServiceUnavailable,
             "recovering",
             "Server is still recovering",
             "after_backoff",
             "process",
             "none",
             {}});
        return out;
    }

    // 5. Authorization scope check.
    if (!out.auth.scopes.has(required_scope)) {
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k403Forbidden,
             "permission_denied",
             "Insufficient scope",
             "never",
             "request",
             "none",
             {}});
        return out;
    }

    // 6. Cookie mutation Origin + CSRF check.
    if (is_mutation && out.auth.via_session_cookie) {
        if (!origin_allowed(cfg, req->getHeader("origin"))) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k403Forbidden,
                 "invalid_origin",
                 "Origin not allowed",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
        if (!auth_state.verify_csrf(out.auth, req->getHeader("rodaq-csrf-token"))) {
            out.error_response = make_error_response(
                out.version, out.request_id,
                {drogon::k403Forbidden,
                 "invalid_csrf_token",
                 "Invalid or missing CSRF token",
                 "never",
                 "request",
                 "none",
                 {}});
            return out;
        }
    }

    // 7. Content-Type check for JSON mutations.
    if (require_json_body && !is_json_content_type(req->getHeader("content-type"))) {
        out.error_response = make_error_response(
            out.version, out.request_id,
            {drogon::k415UnsupportedMediaType,
             "unsupported_media_type",
             "Content-Type must be application/json",
             "never",
             "request",
             "none",
             {}});
        return out;
    }

    out.ok = true;
    return out;
}

Result<PageRequest> parse_page_request(
    const drogon::HttpRequestPtr& req, std::uint32_t max_page_size) {
    PageRequest pr;
    pr.limit = std::min<std::uint32_t>(50, max_page_size);
    const auto cursor = req->getParameter("cursor");
    if (cursor.size() > 512) {
        return Status(StatusCode::InvalidArgument, "Cursor too long");
    }
    pr.cursor = cursor;
    const auto limit_str = req->getParameter("limit");
    if (!limit_str.empty()) {
        auto parsed = parse_decimal_header_u64(limit_str);
        if (!parsed.ok() || parsed.value() == 0 || parsed.value() > max_page_size) {
            return Status(StatusCode::InvalidArgument, "Invalid limit parameter");
        }
        pr.limit = static_cast<std::uint32_t>(parsed.value());
    }
    return pr;
}

Result<ControlPrecondition> parse_control_precondition(const drogon::HttpRequestPtr& req) {
    ControlPrecondition pre;
    const auto hdr = req->getHeader("if-control-generation");
    if (!hdr.empty()) {
        auto gen = parse_decimal_header_u64(hdr);
        if (!gen.ok()) {
            return gen.status();
        }
        pre.has_expected_control_generation = true;
        pre.expected_control_generation = gen.value();
    }
    return pre;
}

}  // namespace

ServerApp::ServerApp() = default;

ServerApp::~ServerApp() noexcept {
    if (started_ && !stopped_) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

void ServerApp::check_web_manifest() {
    web_manifest_ = {};
    if (config_.static_asset_root.empty()) {
        return;
    }
    web_manifest_.checked = true;
    const auto manifest_path = config_.static_asset_root / "web_manifest.json";
    if (!fs::is_regular_file(manifest_path)) {
        web_manifest_.usable = false;
        web_manifest_.error = "Missing web_manifest.json";
        return;
    }
    try {
        std::ifstream in(manifest_path);
        const auto doc = Json::parse(in);
        if (!doc.is_object() || !doc.contains("api_major") || !doc.contains("web_build_id") ||
            !doc["api_major"].is_number_integer() || !doc["web_build_id"].is_string()) {
            web_manifest_.usable = false;
            web_manifest_.error = "Malformed web_manifest.json";
            return;
        }
        const auto api_major = doc["api_major"].get<std::int64_t>();
        web_manifest_.web_build_id = doc["web_build_id"].get<std::string>();
        if (api_major != static_cast<std::int64_t>(kSupportedApiMajor)) {
            web_manifest_.usable = false;
            web_manifest_.error = "Web bundle api_major incompatible with Server API v1";
            return;
        }
        web_manifest_.usable = true;
    } catch (...) {
        web_manifest_.usable = false;
        web_manifest_.error = "Failed to parse web_manifest.json";
    }
}

Result<void> ServerApp::start(ServerConfig config, ServerServices services, bool start_ready) {
    if (started_) {
        return Status(StatusCode::InvalidState, "ServerApp already started");
    }
    auto valid = validate_server_config(config);
    if (!valid.ok()) {
        return valid.status();
    }
    if (!services.tasks || !services.system || !services.artifacts) {
        return Status(StatusCode::InvalidArgument, "ServerServices must be non-null");
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

    config_ = std::move(config);
    services_ = std::move(services);
    auth_ = std::move(auth.value());
    executor_ = std::move(exec.value());
    downloads_ = std::move(dl.value());
    phase_.store(
        start_ready ? ServerPhase::Ready : ServerPhase::Recovering, std::memory_order_release);
    bound_port_ = config_.http.port;
    check_web_manifest();

    auto& app = drogon::app();
    app.setLogLevel(trantor::Logger::kWarn);
    app.setThreadNum(config_.http.worker_threads);
    app.setClientMaxBodySize(config_.http.max_request_body_bytes);
    app.addListener(config_.http.listen_address, config_.http.port);
    app.disableSigtermHandling();

    register_routes();

    std::promise<void> running_promise;
    auto running_future = running_promise.get_future();
    app.getLoop()->queueInLoop([&running_promise]() { running_promise.set_value(); });

    drogon_thread_ = std::thread([]() { drogon::app().run(); });
    running_future.wait();
    started_ = true;
    return {};
}

void ServerApp::mark_ready() noexcept {
    phase_.store(ServerPhase::Ready, std::memory_order_release);
}

void ServerApp::mark_failed() noexcept {
    phase_.store(ServerPhase::Failed, std::memory_order_release);
}

void ServerApp::register_routes() {
    auto& app = drogon::app();

    // --- 1. Session Routes (/api/session) ---
    app.registerHandler(
        "/api/session",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const auto req_id = next_request_id();
            ApiVersion ver{kSupportedApiMajor, kSupportedApiMinor};
            if (!host_allowed(config_, req->getHeader("host"))) {
                cb(make_error_response(
                    ver, req_id,
                    {drogon::k400BadRequest,
                     "invalid_host",
                     "Host not allowed",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            const auto ver_hdr = req->getHeader("rodaq-api-version");
            if (ver_hdr.empty()) {
                cb(make_error_response(
                    ver, req_id,
                    {drogon::k400BadRequest,
                     "missing_api_version",
                     "RoDAQ-API-Version header is required",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            auto parsed_v = parse_api_version_header(ver_hdr);
            if (!parsed_v.ok() || parsed_v.value().major != kSupportedApiMajor) {
                cb(make_error_response(
                    ver, req_id,
                    {drogon::k400BadRequest,
                     "unsupported_api_version",
                     "Unsupported RoDAQ-API-Version",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            ver = parsed_v.value();

            const auto bearer = extract_bearer(req);
            if (!bearer) {
                cb(make_error_response(
                    ver, req_id,
                    {drogon::k401Unauthorized,
                     "unauthenticated",
                     "Authorization Bearer token required",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            auto created = auth_->create_session(*bearer, ver);
            if (!created.ok()) {
                if (created.status().code() == StatusCode::ResourceExhausted) {
                    cb(make_error_response(
                        ver, req_id,
                        {drogon::k429TooManyRequests,
                         "session_capacity_exhausted",
                         "Maximum active sessions reached",
                         "after_backoff",
                         "request",
                         "none",
                         {}}));
                } else {
                    cb(make_error_response(
                        ver, req_id,
                        {drogon::k401Unauthorized,
                         "unauthenticated",
                         "Invalid bearer credential",
                         "never",
                         "request",
                         "none",
                         {}}));
                }
                return;
            }
            auto resp = make_json_response(
                drogon::k200OK, ver, req_id,
                make_success_envelope(ver, req_id, to_session_dto(created.value())), true);
            drogon::Cookie cookie(std::string(kSessionCookieName), created.value().session_id);
            cookie.setHttpOnly(true);
            cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
            cookie.setPath("/");
            resp->addCookie(cookie);
            cb(resp);
        },
        {drogon::Post});

    app.registerHandler(
        "/api/session",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), true, Scope::Read, false, false, true);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            if (!in.auth.via_session_cookie) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k401Unauthorized,
                     "unauthenticated",
                     "Browser session cookie required",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            cb(make_json_response(
                drogon::k200OK, in.version, in.request_id,
                make_success_envelope(in.version, in.request_id, to_session_dto(in.auth)), true));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/session",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), true, Scope::Read, true, false, true);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            if (in.auth.via_session_cookie) {
                auth_->destroy_session(in.auth.session_id);
            }
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k204NoContent);
            drogon::Cookie cleared(std::string(kSessionCookieName), "");
            cleared.setHttpOnly(true);
            cleared.setSameSite(drogon::Cookie::SameSite::kStrict);
            cleared.setPath("/");
            cleared.setMaxAge(0);
            resp->addCookie(cleared);
            apply_common_security_headers(resp, in.version, in.request_id, true);
            cb(resp);
        },
        {drogon::Delete});

    // --- 2. System & Capabilities (/api/system, /api/capabilities) ---
    app.registerHandler(
        "/api/system",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), true, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.health,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Reserve, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id](const auto& claim) {
                    auto snap = services_.system->snapshot();
                    if (!claim()) {
                        return;
                    }
                    if (!snap.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(snap.status(), "process")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(
                            ver, rid,
                            to_system_dto(
                                phase(), snap.value(), downloads_->active_downloads(),
                                web_manifest_.error)),
                        true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "System query timed out",
                         "after_backoff",
                         "process",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id, map_core_status(submitted.status(), "process")));
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/api/capabilities",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), true, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id](const auto& claim) {
                    auto caps = services_.system->capabilities();
                    if (!claim()) {
                        return;
                    }
                    if (!caps.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(caps.status(), "process")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_capabilities_dto(caps.value(), config_)),
                        true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Capabilities query timed out",
                         "after_backoff",
                         "process",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id, map_core_status(submitted.status(), "process")));
            }
        },
        {drogon::Get});

    // --- 3. Task Routes (/api/tasks, /api/tasks/{id}, config, operations, runtime) ---
    app.registerHandler(
        "/api/tasks",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto page_req = parse_page_request(req, config_.http.max_page_size);
            if (!page_req.ok()) {
                cb(make_error_response(
                    in.version, in.request_id, map_core_status(page_req.status(), "request")));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id,
                 pr = std::move(page_req.value())](const auto& claim) {
                    auto page = services_.tasks->list_tasks(pr);
                    if (!claim()) {
                        return;
                    }
                    if (!page.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(page.status(), "task_control")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_task_page_dto(page.value())), true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Task list timed out",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/api/tasks",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Control, true, true);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            const auto idem = req->getHeader("idempotency-key");
            if (!valid_idempotency_key(idem)) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k400BadRequest,
                     "invalid_idempotency_key",
                     "Valid Idempotency-Key header is required",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            auto parsed_cfg = parse_task_config(req->body());
            if (!parsed_cfg.ok()) {
                cb(make_error_response(
                    in.version, in.request_id, map_core_status(parsed_cfg.status(), "request")));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.mutation_acceptance,
                true};
            auto submitted = executor_->submit(
                CallDomain::Control, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id,
                 cfg = std::move(parsed_cfg.value()), key = std::string(idem)](const auto& claim) {
                    auto created = services_.tasks->create_task(cfg, RequestIdentity{key});
                    if (!claim()) {
                        return;
                    }
                    if (!created.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(created.status(), "task_control")));
                        return;
                    }
                    std::string cfg_ver;
                    if (auto snap = services_.tasks->get_task(created.value()); snap.ok()) {
                        cfg_ver = snap.value().active_config_version;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k201Created, ver, rid,
                        make_success_envelope(
                            ver, rid,
                            Json{
                                {"task_id", rodaq::to_string(created.value())},
                                {"config_version", cfg_ver}}),
                        true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Task creation acceptance timed out; retry with same Idempotency-Key",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Post});

    app.registerHandler(
        "/api/tasks/{task_id}",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& task_id_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto tid = parse_task_id(task_id_str);
            if (!tid.ok()) {
                cb(make_error_response(
                    in.version, in.request_id, map_core_status(tid.status(), "request")));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id,
                 id = tid.value()](const auto& claim) {
                    auto snap = services_.tasks->get_task(id);
                    if (!claim()) {
                        return;
                    }
                    if (!snap.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(snap.status(), "task_control")));
                        return;
                    }
                    std::optional<std::string> name;
                    if (auto cfg = services_.tasks->get_active_config(id); cfg.ok()) {
                        name = cfg.value().name;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_task_dto(snap.value(), name)), true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Task query timed out",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Get});

    // Raw Portable TaskConfig Export (NO API envelope; ETag=config_version)
    app.registerHandler(
        "/api/tasks/{task_id}/config",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& task_id_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto tid = parse_task_id(task_id_str);
            if (!tid.ok()) {
                cb(make_error_response(
                    in.version, in.request_id, map_core_status(tid.status(), "request")));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id,
                 id = tid.value()](const auto& claim) {
                    auto cfg = services_.tasks->get_active_config(id);
                    auto snap = services_.tasks->get_task(id);
                    if (!claim()) {
                        return;
                    }
                    if (!cfg.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(cfg.status(), "task_control")));
                        return;
                    }
                    auto exported = export_task_config(cfg.value());
                    if (!exported.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(exported.status(), "task_control")));
                        return;
                    }
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setContentTypeCodeAndCustomString(
                        drogon::CT_APPLICATION_JSON, "application/json; charset=utf-8");
                    resp->setBody(std::move(exported.value()));
                    if (snap.ok()) {
                        resp->addHeader("ETag", "\"" + snap.value().active_config_version + "\"");
                    }
                    apply_common_security_headers(resp, ver, rid, true);
                    (*shared_cb)(resp);
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Config export timed out",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/api/tasks/{task_id}/config",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& task_id_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Control, true, true);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto tid = parse_task_id(task_id_str);
            const auto idem = req->getHeader("idempotency-key");
            auto pre = parse_control_precondition(req);
            auto parsed_cfg = parse_task_config(req->body());
            if (!tid.ok() || !valid_idempotency_key(idem) || !pre.ok() || !parsed_cfg.ok()) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k400BadRequest,
                     "invalid_argument",
                     "Invalid task_id, Idempotency-Key, If-Control-Generation, or TaskConfig",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.mutation_acceptance,
                true};
            auto submitted = executor_->submit(
                CallDomain::Control, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id, id = tid.value(),
                 cfg = std::move(parsed_cfg.value()), key = std::string(idem),
                 precond = pre.value()](const auto& claim) {
                    auto op = services_.tasks->configure(id, cfg, RequestIdentity{key}, precond);
                    if (!claim()) {
                        return;
                    }
                    if (!op.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(op.status(), "task_control")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k202Accepted, ver, rid,
                        make_success_envelope(
                            ver, rid,
                            Json{
                                {"task_id", rodaq::to_string(id)},
                                {"operation_id", rodaq::to_string(op.value())},
                                {"state", "accepted"}}),
                        true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Configure acceptance timed out; retry with same Idempotency-Key",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Put});

    app.registerHandler(
        "/api/tasks/{task_id}/operations",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& task_id_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Control, true, true);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto tid = parse_task_id(task_id_str);
            const auto idem = req->getHeader("idempotency-key");
            auto pre = parse_control_precondition(req);
            auto kind = parse_lifecycle_operation_request(req->body());
            if (!tid.ok() || !valid_idempotency_key(idem) || !pre.ok() || !kind.ok()) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k400BadRequest,
                     "invalid_argument",
                     "Invalid task_id, Idempotency-Key, precondition, or operation body",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            // Reset and Retire require Admin scope.
            if ((kind.value() == OperationKind::Reset || kind.value() == OperationKind::Retire) &&
                !in.auth.scopes.has(Scope::Admin)) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k403Forbidden,
                     "permission_denied",
                     "Admin scope required for reset/retire",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            const auto admission = (kind.value() == OperationKind::Stop) ? AdmissionClass::Reserve
                                                                         : AdmissionClass::Ordinary;
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.mutation_acceptance,
                true};
            auto submitted = executor_->submit(
                CallDomain::Control, admission, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id, id = tid.value(),
                 op_kind = kind.value(), key = std::string(idem),
                 precond = pre.value()](const auto& claim) {
                    Result<OperationId> op = Status(StatusCode::InvalidArgument, {});
                    const RequestIdentity req_id_obj{key};
                    switch (op_kind) {
                        case OperationKind::Start:
                            op = services_.tasks->start(id, req_id_obj, precond);
                            break;
                        case OperationKind::Pause:
                            op = services_.tasks->pause(id, req_id_obj, precond);
                            break;
                        case OperationKind::Resume:
                            op = services_.tasks->resume(id, req_id_obj, precond);
                            break;
                        case OperationKind::Stop:
                            op = services_.tasks->stop(id, req_id_obj, precond);
                            break;
                        case OperationKind::Reset:
                            op = services_.tasks->reset(id, req_id_obj, precond);
                            break;
                        case OperationKind::Retire:
                            op = services_.tasks->retire(id, req_id_obj, precond);
                            break;
                        default:
                            break;
                    }
                    if (!claim()) {
                        return;
                    }
                    if (!op.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(op.status(), "task_control")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k202Accepted, ver, rid,
                        make_success_envelope(
                            ver, rid,
                            Json{
                                {"task_id", rodaq::to_string(id)},
                                {"operation_id", rodaq::to_string(op.value())},
                                {"state", "accepted"}}),
                        true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Operation acceptance timed out; retry with same Idempotency-Key",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Post});

    app.registerHandler(
        "/api/tasks/{task_id}/operations",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& task_id_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto tid = parse_task_id(task_id_str);
            auto page_req = parse_page_request(req, config_.http.max_page_size);
            if (!tid.ok() || !page_req.ok()) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k400BadRequest,
                     "invalid_argument",
                     "Invalid task_id or pagination",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id, id = tid.value(),
                 pr = std::move(page_req.value())](const auto& claim) {
                    auto page = services_.tasks->list_operations(id, pr);
                    if (!claim()) {
                        return;
                    }
                    if (!page.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(page.status(), "task_control")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_operation_page_dto(id, page.value())),
                        true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Operation list timed out",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/api/tasks/{task_id}/operations/{operation_id}",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& task_id_str, const std::string& op_id_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto tid = parse_task_id(task_id_str);
            auto oid = parse_operation_id(op_id_str);
            if (!tid.ok() || !oid.ok()) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k400BadRequest,
                     "invalid_argument",
                     "Invalid task_id or operation_id",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            // Operation settlement lookup uses Query Reserve so ordinary saturation never blocks
            // it.
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Reserve, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id, id = tid.value(),
                 op_id = oid.value()](const auto& claim) {
                    auto op = services_.tasks->get_operation(id, op_id);
                    if (!claim()) {
                        return;
                    }
                    if (!op.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(op.status(), "task_control", true)));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_operation_dto(id, op.value())), true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Operation lookup timed out",
                         "after_backoff",
                         "task_control",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id,
                    map_core_status(submitted.status(), "task_control")));
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/api/tasks/{task_id}/runtime",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb,
            const std::string& task_id_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto tid = parse_task_id(task_id_str);
            if (!tid.ok()) {
                cb(make_error_response(
                    in.version, in.request_id, map_core_status(tid.status(), "request")));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id,
                 id = tid.value()](const auto& claim) {
                    auto rt = services_.tasks->get_runtime_snapshot(id);
                    if (!claim()) {
                        return;
                    }
                    if (!rt.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(rt.status(), "pipeline")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_task_runtime_dto(rt.value())), true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Runtime snapshot timed out",
                         "after_backoff",
                         "pipeline",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id, map_core_status(submitted.status(), "pipeline")));
            }
        },
        {drogon::Get});

    // --- 4. Artifact Routes (/api/artifacts, detail, HEAD/GET files) ---
    app.registerHandler(
        "/api/artifacts",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto page_req = parse_page_request(req, config_.http.max_page_size);
            if (!page_req.ok()) {
                cb(make_error_response(
                    in.version, in.request_id, map_core_status(page_req.status(), "request")));
                return;
            }
            ArtifactQuery query;
            query.limit = page_req.value().limit;
            query.cursor = page_req.value().cursor;
            if (const auto s = req->getParameter("task_id"); !s.empty()) {
                auto tid = parse_task_id(s);
                if (!tid.ok()) {
                    cb(make_error_response(
                        in.version, in.request_id, map_core_status(tid.status(), "request")));
                    return;
                }
                query.task_id = tid.value();
            }
            if (const auto s = req->getParameter("pipeline_id"); !s.empty()) {
                auto pid = parse_decimal_header_u64(s);
                if (!pid.ok()) {
                    cb(make_error_response(
                        in.version, in.request_id, map_core_status(pid.status(), "request")));
                    return;
                }
                query.pipeline_id = pid.value();
            }
            if (const auto s = req->getParameter("format"); !s.empty()) {
                query.format = s;
            }
            const auto rec_begin = req->getParameter("recording_begin");
            const auto rec_end = req->getParameter("recording_end");
            if (!rec_begin.empty() || !rec_end.empty()) {
                if (!query.task_id || rec_begin.empty() || rec_end.empty()) {
                    cb(make_error_response(
                        in.version, in.request_id,
                        {drogon::k400BadRequest,
                         "invalid_argument",
                         "recording_begin and recording_end require task_id",
                         "never",
                         "request",
                         "none",
                         {}}));
                    return;
                }
                auto b = parse_decimal_header_u64(rec_begin);
                auto e = parse_decimal_header_u64(rec_end);
                if (!b.ok() || !e.ok() || b.value() > e.value()) {
                    cb(make_error_response(
                        in.version, in.request_id,
                        {drogon::k400BadRequest,
                         "invalid_argument",
                         "Invalid recording range",
                         "never",
                         "request",
                         "none",
                         {}}));
                    return;
                }
                query.recording_range = RecordingRange{b.value(), e.value()};
            }
            const auto cbeg = req->getParameter("committed_begin");
            const auto cend = req->getParameter("committed_end");
            if (!cbeg.empty() || !cend.empty()) {
                if (cbeg.empty() || cend.empty()) {
                    cb(make_error_response(
                        in.version, in.request_id,
                        {drogon::k400BadRequest,
                         "invalid_argument",
                         "Both committed_begin and committed_end are required",
                         "never",
                         "request",
                         "none",
                         {}}));
                    return;
                }
                auto b = parse_rfc3339_utc(cbeg);
                auto e = parse_rfc3339_utc(cend);
                if (!b.ok() || !e.ok() || b.value() > e.value()) {
                    cb(make_error_response(
                        in.version, in.request_id,
                        {drogon::k400BadRequest,
                         "invalid_argument",
                         "Invalid committed_begin/committed_end",
                         "never",
                         "request",
                         "none",
                         {}}));
                    return;
                }
                query.committed_at = ArtifactWallRange{b.value(), e.value()};
            }

            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id,
                 q = std::move(query)](const auto& claim) {
                    auto page = services_.artifacts->query_artifacts(q);
                    if (!claim()) {
                        return;
                    }
                    if (!page.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(page.status(), "volume")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_artifact_page_dto(page.value())), true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Artifact query timed out",
                         "after_backoff",
                         "volume",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id, map_core_status(submitted.status(), "volume")));
            }
        },
        {drogon::Get});

    app.registerHandler(
        "/api/artifacts/{artifact_id}",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb, const std::string& aid_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto aid = parse_artifact_id(aid_str);
            if (!aid.ok()) {
                cb(make_error_response(
                    in.version, in.request_id, map_core_status(aid.status(), "request")));
                return;
            }
            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id,
                 id = aid.value()](const auto& claim) {
                    auto info = services_.artifacts->get_artifact(id);
                    if (!claim()) {
                        return;
                    }
                    if (!info.ok()) {
                        (*shared_cb)(make_error_response(
                            ver, rid, map_core_status(info.status(), "volume")));
                        return;
                    }
                    (*shared_cb)(make_json_response(
                        drogon::k200OK, ver, rid,
                        make_success_envelope(ver, rid, to_artifact_detail_dto(info.value())),
                        true));
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Artifact detail timed out",
                         "after_backoff",
                         "volume",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id, map_core_status(submitted.status(), "volume")));
            }
        },
        {drogon::Get});

    // HEAD & GET /api/artifacts/{artifact_id}/files/{file_id}
    app.registerHandler(
        "/api/artifacts/{artifact_id}/files/{file_id}",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb, const std::string& aid_str,
            const std::string& fid_str) {
            auto in = run_ingress_pipeline(
                req, config_, *auth_, phase(), false, Scope::Read, false, false, true);
            if (!in.ok) {
                cb(in.error_response);
                return;
            }
            auto aid = parse_artifact_id(aid_str);
            auto fid = parse_artifact_file_id(fid_str);
            if (!aid.ok() || !fid.ok()) {
                cb(make_error_response(
                    in.version, in.request_id,
                    {drogon::k400BadRequest,
                     "invalid_argument",
                     "Invalid artifact_id or file_id",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            const bool is_head = (req->method() == drogon::Head);
            const std::string range_hdr = req->getHeader("range");
            const std::string if_range_hdr = req->getHeader("if-range");

            auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                std::move(cb));
            RequestContext ctx{
                in.request_id,
                in.version,
                in.auth.principal,
                in.auth.scopes,
                std::chrono::steady_clock::now() + config_.timeouts.query,
                false};
            auto submitted = executor_->submit(
                CallDomain::Query, AdmissionClass::Ordinary, ctx,
                [this, shared_cb, ver = in.version, rid = in.request_id, a = aid.value(),
                 f = fid.value(), is_head, range_hdr, if_range_hdr](const auto& claim) {
                    auto info = services_.artifacts->get_artifact(a);
                    if (!info.ok()) {
                        if (claim()) {
                            (*shared_cb)(make_error_response(
                                ver, rid, map_core_status(info.status(), "volume")));
                        }
                        return;
                    }
                    const auto it = std::find_if(
                        info.value().files.begin(), info.value().files.end(),
                        [&](const auto& item) { return item.file_id == f; });
                    if (it == info.value().files.end()) {
                        if (claim()) {
                            (*shared_cb)(make_error_response(
                                ver, rid,
                                {drogon::k404NotFound,
                                 "not_found",
                                 "Artifact file not found",
                                 "never",
                                 "volume",
                                 "none",
                                 {}}));
                        }
                        return;
                    }
                    const auto& file_meta = *it;
                    const std::string strong_etag = "\"" + file_meta.etag + "\"";
                    const std::string safe_name = sanitize_display_filename(file_meta.display_name);
                    const std::string disposition =
                        "attachment; filename=\"" + safe_name + "\"; filename*=UTF-8''" + safe_name;

                    if (is_head) {
                        if (!claim()) {
                            return;
                        }
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setContentTypeCodeAndCustomString(
                            drogon::CT_CUSTOM, file_meta.media_type);
                        resp->addHeader("Accept-Ranges", "bytes");
                        resp->addHeader("Content-Length", std::to_string(file_meta.size));
                        resp->addHeader("ETag", strong_etag);
                        resp->addHeader("Content-Disposition", disposition);
                        resp->addHeader("Cache-Control", "private, no-store");
                        apply_common_security_headers(resp, ver, rid, false);
                        (*shared_cb)(resp);
                        return;
                    }

                    // Acquire committed ReadLease handle before parsing Range / starting stream.
                    auto handle = services_.artifacts->acquire_read(a, f);
                    if (!handle.ok()) {
                        if (claim()) {
                            (*shared_cb)(make_error_response(
                                ver, rid, map_core_status(handle.status(), "volume")));
                        }
                        return;
                    }
                    const auto full_len = handle.value().size();
                    bool apply_range = !range_hdr.empty();
                    if (apply_range && !if_range_hdr.empty() && if_range_hdr != strong_etag) {
                        apply_range = false;
                    }

                    ByteRange selected{0, full_len > 0 ? full_len - 1 : 0};
                    bool partial = false;
                    if (apply_range) {
                        const auto parsed_range = parse_single_byte_range(range_hdr, full_len);
                        if (parsed_range.outcome == RangeParseOutcome::MultipleNotSupported) {
                            if (claim()) {
                                (*shared_cb)(make_error_response(
                                    ver, rid,
                                    {drogon::k400BadRequest,
                                     "multiple_ranges_not_supported",
                                     "Multiple byte ranges are not supported",
                                     "never",
                                     "request",
                                     "none",
                                     {}}));
                            }
                            return;
                        }
                        if (parsed_range.outcome == RangeParseOutcome::Malformed) {
                            if (claim()) {
                                (*shared_cb)(make_error_response(
                                    ver, rid,
                                    {drogon::k400BadRequest,
                                     "invalid_range",
                                     "Malformed Range header",
                                     "never",
                                     "request",
                                     "none",
                                     {}}));
                            }
                            return;
                        }
                        if (parsed_range.outcome == RangeParseOutcome::Unsatisfiable) {
                            if (claim()) {
                                auto resp = make_error_response(
                                    ver, rid,
                                    {drogon::k416RequestedRangeNotSatisfiable,
                                     "range_not_satisfiable",
                                     "Requested range not satisfiable",
                                     "never",
                                     "request",
                                     "none",
                                     {}});
                                resp->addHeader(
                                    "Content-Range", "bytes */" + std::to_string(full_len));
                                (*shared_cb)(resp);
                            }
                            return;
                        }
                        if (parsed_range.outcome == RangeParseOutcome::Valid) {
                            selected = parsed_range.range;
                            partial = true;
                        }
                    }

                    if (!claim()) {
                        return;
                    }

                    if (full_len == 0) {
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k200OK);
                        resp->setContentTypeCodeAndCustomString(
                            drogon::CT_CUSTOM, file_meta.media_type);
                        resp->addHeader("Accept-Ranges", "bytes");
                        resp->addHeader("ETag", strong_etag);
                        resp->addHeader("Content-Disposition", disposition);
                        resp->addHeader("Cache-Control", "private, no-store");
                        apply_common_security_headers(resp, ver, rid, false);
                        (*shared_cb)(resp);
                        return;
                    }

                    auto handle_box = std::make_shared<std::optional<ArtifactReadHandle>>(
                        std::move(handle.value()));
                    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
                        [this, handle_box, selected](drogon::ResponseStreamPtr stream) {
                            if (!handle_box || !handle_box->has_value()) {
                                stream->close();
                                return;
                            }
                            auto h = std::move(**handle_box);
                            handle_box->reset();
                            auto started = downloads_->start_session(
                                std::move(h), selected, std::move(stream));
                            if (!started.ok()) {
                                // Capacity saturated after header send: stream already moved or
                                // closed.
                            }
                        },
                        true);
                    resp->setStatusCode(partial ? drogon::k206PartialContent : drogon::k200OK);
                    resp->setContentTypeCodeAndCustomString(
                        drogon::CT_CUSTOM, file_meta.media_type);
                    resp->addHeader("Accept-Ranges", "bytes");
                    resp->addHeader("ETag", strong_etag);
                    resp->addHeader("Content-Disposition", disposition);
                    resp->addHeader("Cache-Control", "private, no-store");
                    if (partial) {
                        resp->addHeader(
                            "Content-Range", "bytes " + std::to_string(selected.start) + "-" +
                                                 std::to_string(selected.end) + "/" +
                                                 std::to_string(full_len));
                    }
                    apply_common_security_headers(resp, ver, rid, false);
                    (*shared_cb)(resp);
                },
                [shared_cb, ver = in.version, rid = in.request_id]() {
                    (*shared_cb)(make_error_response(
                        ver, rid,
                        {drogon::k504GatewayTimeout,
                         "observer_timeout",
                         "Artifact read setup timed out",
                         "after_backoff",
                         "volume",
                         "none",
                         {}}));
                });
            if (!submitted.ok()) {
                (*shared_cb)(make_error_response(
                    in.version, in.request_id, map_core_status(submitted.status(), "volume")));
            }
        },
        {drogon::Get, drogon::Head});

    // --- 5. Static Web Assets & SPA Fallback (NEVER falls back /api/*) ---
    app.registerHandlerViaRegex(
        "/(?!api(?:/|$)).*",
        [this](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const auto rid = next_request_id();
            const ApiVersion ver{kSupportedApiMajor, kSupportedApiMinor};
            if (config_.static_asset_root.empty() || !web_manifest_.usable) {
                cb(make_error_response(
                    ver, rid,
                    {drogon::k404NotFound,
                     "web_unavailable",
                     web_manifest_.error.value_or("Web console assets not configured"),
                     "never",
                     "process",
                     "none",
                     {}}));
                return;
            }
            std::string rel = req->path();
            if (!rel.empty() && rel.front() == '/') {
                rel.erase(rel.begin());
            }
            if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos) {
                cb(make_error_response(
                    ver, rid,
                    {drogon::k400BadRequest,
                     "invalid_path",
                     "Invalid static asset path",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            fs::path target = rel.empty() ? (config_.static_asset_root / "index.html")
                                          : (config_.static_asset_root / rel);
            bool is_index = rel.empty() || rel == "index.html";
            if (!fs::is_regular_file(target)) {
                target = config_.static_asset_root / "index.html";
                is_index = true;
            }
            if (!fs::is_regular_file(target)) {
                cb(make_error_response(
                    ver, rid,
                    {drogon::k404NotFound,
                     "not_found",
                     "Static asset not found",
                     "never",
                     "request",
                     "none",
                     {}}));
                return;
            }
            std::ifstream in(target, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setContentTypeCodeAndCustomString(
                drogon::CT_CUSTOM, mime_type_for_extension(target));
            if (req->method() != drogon::Head) {
                resp->setBody(ss.str());
            }
            const bool hashed_asset = !is_index && rel.rfind("assets/", 0) == 0;
            resp->addHeader(
                "Cache-Control", hashed_asset ? "public, max-age=31536000, immutable" : "no-cache");
            apply_common_security_headers(resp, ver, rid, false);
            cb(resp);
        },
        {drogon::Get, drogon::Head});

    // Any unmatched /api/* route MUST return structured 404 JSON, never SPA index.html.
    app.registerHandlerViaRegex(
        "/api/.*",
        [](const drogon::HttpRequestPtr& /*req*/,
           std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
            const auto rid = next_request_id();
            cb(make_error_response(
                ApiVersion{kSupportedApiMajor, kSupportedApiMinor}, rid,
                {drogon::k404NotFound,
                 "not_found",
                 "API route not found",
                 "never",
                 "request",
                 "none",
                 {}}));
        },
        {drogon::Get, drogon::Post, drogon::Put, drogon::Delete, drogon::Head, drogon::Patch});
}

Result<void> ServerApp::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (!started_ || stopped_) {
        return {};
    }
    phase_.store(ServerPhase::Draining, std::memory_order_release);

    if (downloads_) {
        downloads_->cancel_all();
    }
    drogon::app().quit();
    if (drogon_thread_.joinable()) {
        drogon_thread_.join();
    }

    // Parallel convergence of active DownloadSession drain and Core process shutdown drain.
    std::future<Result<void>> dl_future = std::async(std::launch::async, [this, deadline]() {
        return downloads_ ? downloads_->shutdown(deadline) : Result<void>{};
    });
    Result<void> core_res;
    if (services_.core_shutdown) {
        core_res = services_.core_shutdown(deadline);
    }
    const auto dl_res = dl_future.get();
    const auto exec_res = executor_ ? executor_->shutdown(deadline) : Result<void>{};

    stopped_ = true;
    if (!core_res.ok()) {
        return core_res;
    }
    if (!dl_res.ok()) {
        return dl_res;
    }
    return exec_res;
}

ExecutorMetrics ServerApp::executor_metrics() const noexcept {
    return executor_ ? executor_->metrics() : ExecutorMetrics{};
}

std::size_t ServerApp::active_sessions() const noexcept {
    return auth_ ? auth_->active_sessions() : 0;
}

std::size_t ServerApp::active_downloads() const noexcept {
    return downloads_ ? downloads_->active_downloads() : 0;
}

}  // namespace rodaq::server
