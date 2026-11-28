#include "server_app.hpp"

#include "api/dto.hpp"
#include "api/error_response.hpp"
#include "api/json_codec.hpp"
#include "download/download_session.hpp"
#include "security/auth.hpp"
#include "static_assets.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <drogon/drogon.h>
#include <rodaq/logging.hpp>

namespace rodaq::server {
namespace {

constexpr const char* kCspHeaderValue =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
    "font-src 'self'; connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri "
    "'none'";

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::optional<ApiVersion> parse_api_version_header(std::string_view raw) noexcept {
    const auto dot = raw.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= raw.size() ||
        raw.find('.', dot + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto maj = parse_u64_decimal(raw.substr(0, dot));
    const auto min = parse_u64_decimal(raw.substr(dot + 1));
    if (!maj || !min || *maj == 0 || *maj > 1000000 || *min > 1000000) {
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

bool is_json_content_type(std::string_view ct) {
    const auto semi = ct.find(';');
    std::string media = to_lower_ascii(semi == std::string_view::npos ? ct : ct.substr(0, semi));
    while (!media.empty() && (media.front() == ' ' || media.front() == '\t')) {
        media.erase(media.begin());
    }
    while (!media.empty() && (media.back() == ' ' || media.back() == '\t')) {
        media.pop_back();
    }
    return media == "application/json";
}

std::size_t estimate_header_bytes(const drogon::HttpRequestPtr& req) {
    std::size_t total = req->path().size() + req->query().size();
    for (const auto& kv : req->headers()) {
        total += kv.first.size() + kv.second.size() + 4;
    }
    return total;
}

void apply_common_headers(
    const drogon::HttpResponsePtr& resp, const std::string& request_id,
    ApiVersion ver = {kApiMajor, kApiMinor}) {
    resp->addHeader(
        "RoDAQ-API-Version", std::to_string(ver.major) + "." + std::to_string(ver.minor));
    resp->addHeader("RoDAQ-Request-ID", request_id);
    resp->addHeader("Content-Security-Policy", kCspHeaderValue);
    resp->addHeader("Referrer-Policy", "no-referrer");
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("X-Frame-Options", "DENY");
}

drogon::HttpResponsePtr make_json_response(
    int http_status, const nlohmann::json& body, const std::string& request_id,
    std::size_t max_response_bytes, ApiVersion ver = {kApiMajor, kApiMinor},
    std::optional<std::uint32_t> retry_after = std::nullopt) {
    std::string dumped = body.dump();
    if (dumped.size() > max_response_bytes) {
        ApiError overflow;
        overflow.http_status = 500;
        overflow.code = "response_too_large";
        overflow.message = "serialized response exceeded max_json_response_bytes";
        overflow.failure_domain = FailureDomain::Process;
        overflow.retryability = Retryability::AfterConditionChange;
        http_status = 500;
        dumped = make_error_envelope(request_id, std::move(overflow), ver).dump();
    }
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(http_status));
    resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM, "application/json; charset=utf-8");
    resp->setBody(std::move(dumped));
    resp->addHeader("Cache-Control", "no-store");
    if (retry_after) {
        resp->addHeader("Retry-After", std::to_string(*retry_after));
    }
    apply_common_headers(resp, request_id, ver);
    return resp;
}

drogon::HttpResponsePtr make_error_http(
    ApiError err, const std::string& request_id, std::size_t max_response_bytes,
    ApiVersion ver = {kApiMajor, kApiMinor}) {
    const int status = err.http_status;
    const auto retry_after = err.retry_after_seconds;
    return make_json_response(
        status, make_error_envelope(request_id, std::move(err), ver), request_id,
        max_response_bytes, ver, retry_after);
}

std::string new_request_id() {
    static std::atomic<std::uint64_t> fallback_seq{1};
    auto rand = generate_random_hex(12);
    if (rand.ok()) {
        return "req-" + rand.value();
    }
    return "req-" + std::to_string(fallback_seq.fetch_add(1, std::memory_order_relaxed));
}

struct GuardOptions {
    bool is_mutation{false};
    bool expect_json_body{false};
    bool allow_in_draining{false};
};

struct RequestGuardResult {
    bool ok{false};
    std::string request_id;
    ApiVersion api_version{kApiMajor, kApiMinor};
    drogon::HttpResponsePtr rejection;
};

}  // namespace

struct ServerApp::Impl {
    ServerConfig config;
    ServerServices services;
    std::atomic<ServerPhase> phase{ServerPhase::Starting};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    std::unique_ptr<BearerAuthenticator> auth;
    std::unique_ptr<ControlExecutor> executor;
    std::unique_ptr<DownloadRegistry> downloads;
    StaticAssetService static_assets;
    std::thread drogon_thread;

    JsonParseLimits json_limits() const noexcept {
        return JsonParseLimits{config.http.max_request_body_bytes, config.http.max_json_depth};
    }

    RequestGuardResult guard_request(
        const drogon::HttpRequestPtr& req, const GuardOptions& opts) const {
        RequestGuardResult res;
        res.request_id = new_request_id();
        const auto max_resp = config.http.max_json_response_bytes;

        // 1. Hard request size / body rules.
        if (estimate_header_bytes(req) > config.http.max_header_bytes) {
            res.rejection = make_error_http(
                make_request_error(400, "header_too_large", "HTTP headers exceed configured limit"),
                res.request_id, max_resp);
            return res;
        }
        if (req->query().size() > config.http.max_query_bytes) {
            res.rejection = make_error_http(
                make_request_error(400, "query_too_large", "query string exceeds configured limit"),
                res.request_id, max_resp);
            return res;
        }
        if (req->body().size() > config.http.max_request_body_bytes) {
            res.rejection = make_error_http(
                make_request_error(413, "payload_too_large", "request body exceeds limit"),
                res.request_id, max_resp);
            return res;
        }
        if ((req->method() == drogon::Get || req->method() == drogon::Head) &&
            !req->body().empty()) {
            res.rejection = make_error_http(
                make_request_error(400, "unexpected_body", "GET/HEAD requests must not carry body"),
                res.request_id, max_resp);
            return res;
        }
        if (opts.expect_json_body && !is_json_content_type(req->getHeader("content-type"))) {
            res.rejection = make_error_http(
                make_request_error(
                    415, "unsupported_media_type", "Content-Type must be application/json"),
                res.request_id, max_resp);
            return res;
        }

        // 2. Host check (exact canonical loopback allowlist).
        const std::string host = to_lower_ascii(req->getHeader("host"));
        if (config.allowed_hosts.find(host) == config.allowed_hosts.end()) {
            RODAQ_LOG_WARNING("server security_reject_host request_id={}", res.request_id);
            res.rejection = make_error_http(
                make_request_error(403, "invalid_host", "Host header is not an allowed loopback"),
                res.request_id, max_resp);
            return res;
        }

        // 3. API version header required on all /api/ requests.
        const std::string version_hdr = req->getHeader("rodaq-api-version");
        if (version_hdr.empty()) {
            res.rejection = make_error_http(
                make_request_error(
                    400, "missing_api_version", "RoDAQ-API-Version header is required"),
                res.request_id, max_resp);
            return res;
        }
        const auto parsed_ver = parse_api_version_header(version_hdr);
        if (!parsed_ver || parsed_ver->major != kApiMajor) {
            res.rejection = make_error_http(
                make_request_error(
                    400, "unsupported_api_version", "API version is malformed or unsupported"),
                res.request_id, max_resp);
            return res;
        }
        res.api_version = *parsed_ver;

        // 4. Bearer token authentication (constant-time).
        const auto bearer = extract_bearer_token(req->getHeader("authorization"));
        if (!bearer || !auth->authenticate(*bearer)) {
            RODAQ_LOG_WARNING("server auth_failure request_id={}", res.request_id);
            res.rejection = make_error_http(
                make_request_error(401, "unauthenticated", "valid Bearer token required"),
                res.request_id, max_resp, res.api_version);
            return res;
        }

        // 5. Origin check: reject any non-matching Origin header (and always check on mutations).
        const std::string origin = req->getHeader("origin");
        if (!origin.empty() &&
            config.allowed_origins.find(origin) == config.allowed_origins.end()) {
            RODAQ_LOG_WARNING("server security_reject_origin request_id={}", res.request_id);
            res.rejection = make_error_http(
                make_request_error(403, "invalid_origin", "Origin header is not allowed"),
                res.request_id, max_resp, res.api_version);
            return res;
        }

        // 6. Server lifecycle phase check.
        const ServerPhase cur = phase.load(std::memory_order_acquire);
        if (cur == ServerPhase::Draining && !opts.allow_in_draining) {
            res.rejection = make_error_http(
                make_request_error(503, "draining", "server is draining for shutdown"),
                res.request_id, max_resp, res.api_version);
            return res;
        }
        if (cur == ServerPhase::Starting || cur == ServerPhase::Failed) {
            res.rejection = make_error_http(
                make_request_error(503, "unavailable", "server is not ready"), res.request_id,
                max_resp, res.api_version);
            return res;
        }

        (void)opts.is_mutation;
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

    void dispatch_control(
        const RequestGuardResult& guard,
        std::function<void(const drogon::HttpResponsePtr&)> http_cb,
        std::function<drogon::HttpResponsePtr()> work_fn) {
        auto shared_cb = std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
            std::move(http_cb));
        const std::string req_id = guard.request_id;
        const ApiVersion ver = guard.api_version;
        const auto max_resp = config.http.max_json_response_bytes;

        auto submitted = executor->submit(
            config.timeouts.mutation_acceptance,
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
                        "observer deadline exceeded; refresh authoritative Task state"),
                    req_id, max_resp, ver));
            });

        if (!submitted.admitted) {
            (*shared_cb)(make_error_http(
                make_request_error(
                    503, "server_busy", "control execution capacity temporarily saturated"),
                req_id, max_resp, ver));
        }
    }

    void register_routes() {
        auto& app = drogon::app();

        // GET /api/system (reads published snapshot directly, non-blocking)
        app.registerHandler(
            "/api/system",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
                opts.allow_in_draining = true;
                auto g = guard_request(req, opts);
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto snap = services.system->snapshot();
                if (!snap.ok()) {
                    cb(make_error_http(
                        map_core_status(snap.status(), FailureDomain::Process), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto data = serialize_system(
                    phase.load(std::memory_order_acquire), snap.value(), downloads->active_count(),
                    static_assets.deployment_error());
                cb(make_json_response(
                    200, make_success_envelope(g.request_id, std::move(data), g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // GET /api/capabilities
        app.registerHandler(
            "/api/capabilities",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                auto g = guard_request(req, {});
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto caps = services.system->capabilities();
                auto snap = services.system->snapshot();
                if (!caps.ok()) {
                    cb(make_error_http(
                        map_core_status(caps.status(), FailureDomain::Process), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                SystemSnapshot sys_snap = snap.ok() ? std::move(snap.value()) : SystemSnapshot{};
                auto data = serialize_capabilities(caps.value(), sys_snap, config);
                cb(make_json_response(
                    200, make_success_envelope(g.request_id, std::move(data), g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // GET /api/tasks
        app.registerHandler(
            "/api/tasks",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                auto g = guard_request(req, {});
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                PageRequest page_req;
                drogon::HttpResponsePtr err_resp;
                if (!parse_page_request(
                        req, g.request_id, g.api_version, page_req.limit, page_req.cursor,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                auto listed = services.tasks->list_tasks(page_req);
                if (!listed.ok()) {
                    cb(make_error_http(
                        map_core_status(listed.status(), FailureDomain::TaskControl), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                cb(make_json_response(
                    200,
                    make_success_envelope(
                        g.request_id, serialize_task_page(listed.value()), g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // POST /api/tasks (bounded ControlExecutor)
        app.registerHandler(
            "/api/tasks",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                GuardOptions opts;
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
                        req, g.request_id, g.api_version, req_id_obj, ignored_pre, false,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                const std::string body(req->body());
                const auto parsed_json = parse_bounded_json(body, json_limits());
                if (!parsed_json.ok) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_json", parsed_json.error.message,
                            {{parsed_json.error.code, parsed_json.error.path,
                              parsed_json.error.message}}),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto parsed_cfg = parse_task_config(body);
                if (!parsed_cfg.ok()) {
                    cb(make_error_http(
                        map_core_status(parsed_cfg.status(), FailureDomain::Request), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                dispatch_control(
                    g, std::move(cb),
                    [this, cfg = std::move(parsed_cfg.value()), req_id_obj = std::move(req_id_obj),
                     req_id = g.request_id, ver = g.api_version]() {
                        auto created = services.tasks->create_task(cfg, req_id_obj);
                        if (!created.ok()) {
                            RODAQ_LOG_WARNING("server task_create_failed request_id={}", req_id);
                            return make_error_http(
                                map_core_status(created.status(), FailureDomain::TaskControl),
                                req_id, config.http.max_json_response_bytes, ver);
                        }
                        std::string config_version;
                        if (auto snap = services.tasks->get_task(created.value()); snap.ok()) {
                            config_version = snap.value().active_config_version;
                        }
                        RODAQ_LOG_INFO(
                            "server task_created request_id={} task_id={}", req_id,
                            to_string(created.value()));
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
                auto g = guard_request(req, {});
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
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto snap = services.tasks->get_task(parsed_id.value());
                if (!snap.ok()) {
                    cb(make_error_http(
                        map_core_status(snap.status(), FailureDomain::TaskControl), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                cb(make_json_response(
                    200,
                    make_success_envelope(
                        g.request_id, serialize_task_data(snap.value()), g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // GET /api/tasks/{task_id}/config (raw Portable TaskConfig without API envelope)
        app.registerHandler(
            "/api/tasks/{1}/config",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                auto g = guard_request(req, {});
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_id = parse_task_id(raw_task_id);
                if (!parsed_id.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto cfg = services.tasks->get_active_config(parsed_id.value());
                if (!cfg.ok()) {
                    cb(make_error_http(
                        map_core_status(cfg.status(), FailureDomain::TaskControl), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto exported = export_task_config(cfg.value());
                if (!exported.ok()) {
                    cb(make_error_http(
                        map_core_status(exported.status(), FailureDomain::TaskControl),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto snap = services.tasks->get_task(parsed_id.value());
                auto resp = drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(drogon::k200OK);
                resp->setContentTypeCodeAndCustomString(
                    drogon::CT_CUSTOM, "application/json; charset=utf-8");
                resp->setBody(std::move(exported.value()));
                resp->addHeader("Cache-Control", "no-store");
                if (snap.ok() && !snap.value().active_config_version.empty()) {
                    resp->addHeader("ETag", "\"" + snap.value().active_config_version + "\"");
                }
                apply_common_headers(resp, g.request_id, g.api_version);
                cb(resp);
            },
            {drogon::Get});

        // PUT /api/tasks/{task_id}/config (bounded ControlExecutor)
        app.registerHandler(
            "/api/tasks/{1}/config",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
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
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                RequestIdentity req_id_obj;
                ControlPrecondition precondition;
                drogon::HttpResponsePtr err_resp;
                if (!parse_idempotency_and_precondition(
                        req, g.request_id, g.api_version, req_id_obj, precondition, true,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                const std::string body(req->body());
                const auto parsed_json = parse_bounded_json(body, json_limits());
                if (!parsed_json.ok) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_json", parsed_json.error.message,
                            {{parsed_json.error.code, parsed_json.error.path,
                              parsed_json.error.message}}),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto parsed_cfg = parse_task_config(body);
                if (!parsed_cfg.ok()) {
                    cb(make_error_http(
                        map_core_status(parsed_cfg.status(), FailureDomain::Request), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                dispatch_control(
                    g, std::move(cb),
                    [this, tid = parsed_id.value(), cfg = std::move(parsed_cfg.value()),
                     req_id_obj = std::move(req_id_obj), precondition, req_id = g.request_id,
                     ver = g.api_version]() {
                        auto op = services.tasks->configure(tid, cfg, req_id_obj, precondition);
                        if (!op.ok()) {
                            RODAQ_LOG_WARNING(
                                "server task_configure_failed request_id={} task_id={}", req_id,
                                to_string(tid));
                            return make_error_http(
                                map_core_status(op.status(), FailureDomain::TaskControl), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        RODAQ_LOG_INFO(
                            "server task_configure_accepted request_id={} operation_id={}", req_id,
                            to_string(op.value()));
                        return make_json_response(
                            202,
                            make_success_envelope(
                                req_id, serialize_mutation_accepted(tid, op.value()), ver),
                            req_id, config.http.max_json_response_bytes, ver);
                    });
            },
            {drogon::Put});

        // POST /api/tasks/{task_id}/operations (bounded ControlExecutor)
        app.registerHandler(
            "/api/tasks/{1}/operations",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                GuardOptions opts;
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
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                RequestIdentity req_id_obj;
                ControlPrecondition precondition;
                drogon::HttpResponsePtr err_resp;
                if (!parse_idempotency_and_precondition(
                        req, g.request_id, g.api_version, req_id_obj, precondition, true,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                ApiError dto_error;
                auto dto = parse_operation_request(req->body(), json_limits(), dto_error);
                if (!dto.ok()) {
                    cb(make_error_http(
                        dto_error, g.request_id, config.http.max_json_response_bytes,
                        g.api_version));
                    return;
                }
                const auto kind = dto.value().kind;
                dispatch_control(
                    g, std::move(cb),
                    [this, tid = parsed_id.value(), kind, req_id_obj = std::move(req_id_obj),
                     precondition, req_id = g.request_id, ver = g.api_version]() {
                        Result<OperationId> op(Status(StatusCode::Internal, "unhandled kind"));
                        switch (kind) {
                            case OperationKind::Start:
                                op = services.tasks->start(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Pause:
                                op = services.tasks->pause(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Resume:
                                op = services.tasks->resume(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Stop:
                                op = services.tasks->stop(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Reset:
                                op = services.tasks->reset(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Retire:
                                op = services.tasks->retire(tid, req_id_obj, precondition);
                                break;
                            case OperationKind::Configure:
                                break;
                        }
                        if (!op.ok()) {
                            RODAQ_LOG_WARNING(
                                "server task_op_failed request_id={} kind={} task_id={}", req_id,
                                to_wire_string(kind), to_string(tid));
                            return make_error_http(
                                map_core_status(op.status(), FailureDomain::TaskControl), req_id,
                                config.http.max_json_response_bytes, ver);
                        }
                        RODAQ_LOG_INFO(
                            "server task_op_accepted request_id={} kind={} operation_id={}", req_id,
                            to_wire_string(kind), to_string(op.value()));
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
                auto g = guard_request(req, {});
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_id = parse_task_id(raw_task_id);
                if (!parsed_id.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                PageRequest page_req;
                drogon::HttpResponsePtr err_resp;
                if (!parse_page_request(
                        req, g.request_id, g.api_version, page_req.limit, page_req.cursor,
                        err_resp)) {
                    cb(err_resp);
                    return;
                }
                auto page = services.tasks->list_operations(parsed_id.value(), page_req);
                if (!page.ok()) {
                    cb(make_error_http(
                        map_core_status(page.status(), FailureDomain::TaskControl), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                cb(make_json_response(
                    200,
                    make_success_envelope(
                        g.request_id, serialize_operation_page(parsed_id.value(), page.value()),
                        g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // GET /api/tasks/{task_id}/operations/{operation_id}
        app.registerHandler(
            "/api/tasks/{1}/operations/{2}",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id, const std::string& raw_op_id) {
                GuardOptions opts;
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
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto op = services.tasks->get_operation(parsed_tid.value(), parsed_oid.value());
                if (!op.ok()) {
                    cb(make_error_http(
                        map_core_status(op.status(), FailureDomain::TaskControl, true),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                cb(make_json_response(
                    200,
                    make_success_envelope(
                        g.request_id, serialize_operation(parsed_tid.value(), op.value()),
                        g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // GET /api/tasks/{task_id}/runtime
        app.registerHandler(
            "/api/tasks/{1}/runtime",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_task_id) {
                auto g = guard_request(req, {});
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto parsed_tid = parse_task_id(raw_task_id);
                if (!parsed_tid.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_task_id", "malformed task_id"),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto rt = services.tasks->get_runtime_snapshot(parsed_tid.value());
                if (!rt.ok()) {
                    cb(make_error_http(
                        map_core_status(rt.status(), FailureDomain::Pipeline), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                cb(make_json_response(
                    200,
                    make_success_envelope(
                        g.request_id, serialize_task_runtime(rt.value()), g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // GET /api/artifacts
        app.registerHandler(
            "/api/artifacts",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
                auto g = guard_request(req, {});
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                ArtifactQuery query;
                drogon::HttpResponsePtr err_resp;
                if (!parse_page_request(
                        req, g.request_id, g.api_version, query.limit, query.cursor, err_resp)) {
                    cb(err_resp);
                    return;
                }
                const auto invalid_filter = [&](std::string msg) {
                    cb(make_error_http(
                        make_request_error(
                            400, "invalid_query", std::move(msg), {{"invalid_format", "", ""}}),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
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
                auto page = services.artifacts->query_artifacts(query);
                if (!page.ok()) {
                    cb(make_error_http(
                        map_core_status(page.status(), FailureDomain::Storage), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                cb(make_json_response(
                    200,
                    make_success_envelope(
                        g.request_id, serialize_artifact_page(page.value()), g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // GET /api/artifacts/{artifact_id}
        app.registerHandler(
            "/api/artifacts/{1}",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_art_id) {
                auto g = guard_request(req, {});
                if (!g.ok) {
                    cb(g.rejection);
                    return;
                }
                auto aid = parse_artifact_id(raw_art_id);
                if (!aid.ok()) {
                    cb(make_error_http(
                        make_request_error(400, "invalid_artifact_id", "malformed artifact_id"),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                auto info = services.artifacts->get_artifact(aid.value());
                if (!info.ok()) {
                    cb(make_error_http(
                        map_core_status(info.status(), FailureDomain::Storage), g.request_id,
                        config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                cb(make_json_response(
                    200,
                    make_success_envelope(
                        g.request_id, serialize_artifact_detail(info.value()), g.api_version),
                    g.request_id, config.http.max_json_response_bytes, g.api_version));
            },
            {drogon::Get});

        // HEAD / GET /api/artifacts/{artifact_id}/files/{file_id}
        app.registerHandler(
            "/api/artifacts/{1}/files/{2}",
            [this](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                const std::string& raw_art_id, const std::string& raw_file_id) {
                auto g = guard_request(req, {});
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
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                    return;
                }
                if (req->isHead()) {
                    auto info = services.artifacts->get_artifact(aid.value());
                    if (!info.ok()) {
                        cb(make_error_http(
                            map_core_status(info.status(), FailureDomain::Storage), g.request_id,
                            config.http.max_json_response_bytes, g.api_version));
                        return;
                    }
                    const ArtifactFileInfo* matched = nullptr;
                    for (const auto& f : info.value().files) {
                        if (f.file_id == fid.value()) {
                            matched = &f;
                            break;
                        }
                    }
                    if (!matched) {
                        cb(make_error_http(
                            make_request_error(404, "not_found", "artifact file not found"),
                            g.request_id, config.http.max_json_response_bytes, g.api_version));
                        return;
                    }
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->setPassThrough(true);
                    resp->addHeader("Content-Type", matched->media_type);
                    resp->addHeader("Accept-Ranges", "bytes");
                    resp->addHeader("Content-Length", std::to_string(matched->size));
                    resp->addHeader("ETag", "\"" + matched->etag + "\"");
                    resp->addHeader(
                        "Content-Disposition", build_content_disposition(matched->display_name));
                    resp->addHeader("Cache-Control", "private, no-store");
                    if (req->getHeader("connection") == "close") {
                        resp->setCloseConnection(true);
                        resp->addHeader("Connection", "close");
                    }
                    apply_common_headers(resp, g.request_id, g.api_version);
                    cb(resp);
                    return;
                }

                const std::string range_hdr = req->getHeader("range");
                const std::string if_range_hdr = req->getHeader("if-range");
                const auto conn_weak = req->getConnectionPtr();
                auto shared_cb =
                    std::make_shared<std::function<void(const drogon::HttpResponsePtr&)>>(
                        std::move(cb));

                // Acquire ArtifactReadHandle on bounded download disk worker (off event loop).
                const bool queued = downloads->submit_open([this, art_id = aid.value(),
                                                            file_id = fid.value(), range_hdr,
                                                            if_range_hdr, conn_weak, shared_cb,
                                                            req_id = g.request_id,
                                                            ver = g.api_version]() {
                    auto info = services.artifacts->get_artifact(art_id);
                    if (!info.ok()) {
                        (*shared_cb)(make_error_http(
                            map_core_status(info.status(), FailureDomain::Storage), req_id,
                            config.http.max_json_response_bytes, ver));
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
                        (*shared_cb)(make_error_http(
                            make_request_error(404, "not_found", "artifact file not found"), req_id,
                            config.http.max_json_response_bytes, ver));
                        return;
                    }
                    auto acquired = services.artifacts->acquire_read(art_id, file_id);
                    if (!acquired.ok()) {
                        (*shared_cb)(make_error_http(
                            map_core_status(acquired.status(), FailureDomain::Storage), req_id,
                            config.http.max_json_response_bytes, ver));
                        return;
                    }
                    auto handle = std::make_shared<ArtifactReadHandle>(std::move(acquired.value()));
                    const std::uint64_t full_length = handle->size();

                    std::string effective_range = range_hdr;
                    if (!effective_range.empty() && !if_range_hdr.empty() &&
                        !if_range_matches_etag(if_range_hdr, matched->etag)) {
                        effective_range.clear();
                    }
                    const auto parsed_range = parse_single_byte_range(effective_range, full_length);
                    if (parsed_range.status == RangeParseStatus::MultipleRanges) {
                        (*shared_cb)(make_error_http(
                            make_request_error(
                                400, "multiple_ranges_not_supported",
                                "V1 supports at most one byte-range-spec"),
                            req_id, config.http.max_json_response_bytes, ver));
                        return;
                    }
                    if (parsed_range.status == RangeParseStatus::Malformed) {
                        (*shared_cb)(make_error_http(
                            make_request_error(400, "invalid_range", "malformed Range header"),
                            req_id, config.http.max_json_response_bytes, ver));
                        return;
                    }
                    if (parsed_range.status == RangeParseStatus::Unsatisfiable) {
                        auto resp = make_error_http(
                            make_request_error(
                                416, "range_not_satisfiable", "requested Range unsatisfiable"),
                            req_id, config.http.max_json_response_bytes, ver);
                        resp->addHeader("Content-Range", "bytes */" + std::to_string(full_length));
                        (*shared_cb)(resp);
                        return;
                    }

                    auto conn = conn_weak.lock();
                    if (!conn || conn->disconnected()) {
                        return;
                    }
                    auto admitted =
                        downloads->create_session(parsed_range.range, std::move(handle), conn_weak);
                    if (!admitted.admitted) {
                        (*shared_cb)(make_error_http(
                            make_request_error(
                                503, "downloads_busy", "download concurrency limit reached"),
                            req_id, config.http.max_json_response_bytes, ver));
                        return;
                    }

                    RODAQ_LOG_INFO(
                        "server download_started request_id={} artifact={}/{}", req_id,
                        to_string(art_id), to_string(file_id));

                    std::string header_block;
                    header_block.reserve(512);
                    header_block += parsed_range.range.partial ? "HTTP/1.1 206 Partial Content\r\n"
                                                               : "HTTP/1.1 200 OK\r\n";
                    header_block += "Content-Type: " + matched->media_type + "\r\n";
                    header_block += "Accept-Ranges: bytes\r\n";
                    header_block +=
                        "Content-Length: " + std::to_string(parsed_range.range.length) + "\r\n";
                    header_block += "ETag: \"" + matched->etag + "\"\r\n";
                    header_block +=
                        "Content-Disposition: " + build_content_disposition(matched->display_name) +
                        "\r\n";
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
                });

                if (!queued) {
                    (*shared_cb)(make_error_http(
                        make_request_error(
                            503, "downloads_busy", "download capacity temporarily saturated"),
                        g.request_id, config.http.max_json_response_bytes, g.api_version));
                }
            },
            {drogon::Get, drogon::Head});

        // Static SPA / 404 handler (never falls back /api/* to index.html)
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

Result<void> ServerApp::start(ServerConfig config, ServerServices services) {
    if (impl_->started.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "ServerApp already started");
    }
    if (!services.tasks || !services.system || !services.artifacts) {
        return Status(StatusCode::InvalidArgument, "ServerServices pointers must be non-null");
    }
    auto valid = validate_server_config(config);
    if (!valid.ok()) {
        return valid.status();
    }
    auto auth = BearerAuthenticator::create(config.security);
    if (!auth.ok()) {
        return auth.status();
    }
    auto exec = ControlExecutor::create(config.execution);
    if (!exec.ok()) {
        return exec.status();
    }
    auto dl = DownloadRegistry::create(config.download);
    if (!dl.ok()) {
        return dl.status();
    }

    impl_->config = std::move(config);
    impl_->services = services;
    impl_->auth = std::move(auth.value());
    impl_->executor = std::move(exec.value());
    impl_->downloads = std::move(dl.value());
    impl_->static_assets = StaticAssetService::create(impl_->config.static_assets_root, kApiMajor);
    impl_->phase.store(ServerPhase::Ready, std::memory_order_release);

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
        impl_->phase.store(ServerPhase::Failed, std::memory_order_release);
        return Status(StatusCode::Unavailable, "Drogon event loop did not start within 5s");
    }
    RODAQ_LOG_INFO(
        "server started listen={}:{} phase=ready", impl_->config.http.listen_address,
        impl_->config.http.port);
    return {};
}

Result<void> ServerApp::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (!impl_->started.load(std::memory_order_acquire) ||
        impl_->stopped.exchange(true, std::memory_order_acq_rel)) {
        return {};
    }
    impl_->phase.store(ServerPhase::Draining, std::memory_order_release);
    RODAQ_LOG_INFO("server entering phase=draining");

    if (impl_->downloads) {
        impl_->downloads->cancel_all();
    }
    Result<void> dl_res;
    if (impl_->downloads) {
        dl_res = impl_->downloads->shutdown(deadline);
    }
    Result<void> exec_res;
    if (impl_->executor) {
        exec_res = impl_->executor->shutdown(deadline);
    }
    drogon::app().quit();
    if (impl_->drogon_thread.joinable()) {
        impl_->drogon_thread.join();
    }
    if (!dl_res.ok()) {
        return dl_res.status();
    }
    if (!exec_res.ok()) {
        return exec_res.status();
    }
    RODAQ_LOG_INFO("server shutdown settled");
    return {};
}

ServerPhase ServerApp::phase() const noexcept {
    return impl_->phase.load(std::memory_order_acquire);
}

ControlMetrics ServerApp::control_metrics() const {
    return impl_->executor ? impl_->executor->metrics() : ControlMetrics{};
}

DownloadMetrics ServerApp::download_metrics() const {
    return impl_->downloads ? impl_->downloads->metrics() : DownloadMetrics{};
}

}  // namespace rodaq::server
