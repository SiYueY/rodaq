#include "api/dto.hpp"
#include "api/error_response.hpp"
#include "api/json_codec.hpp"
#include "config.hpp"
#include "download/download_registry.hpp"
#include "download/download_session.hpp"
#include "security/auth.hpp"
#include "service_call_executor.hpp"
#include "static_assets.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using namespace rodaq::server;

struct TempDir {
    fs::path path;

    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("rodaq-server-unit-" + tag + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file_mode(const fs::path& p, const std::string& content, mode_t mode) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
    f.close();
    ASSERT_EQ(::chmod(p.c_str(), mode), 0);
}

TEST(ServerConfigTest, ParsesDefaultsAndRejectsNonLoopbackOrUnknownFields) {
    const std::string valid_json = R"({
        "format_version": "1.0",
        "http": { "listen_address": "127.0.0.1", "port": 18080 },
        "security": { "credential_store": "/tmp/rodaq_creds.json" }
    })";
    auto cfg = parse_server_config_json(valid_json);
    ASSERT_TRUE(cfg.ok()) << cfg.status().message();
    EXPECT_EQ(cfg.value().http.listen_address, "127.0.0.1");
    EXPECT_EQ(cfg.value().http.port, 18080);
    EXPECT_EQ(cfg.value().allowed_hosts.count("127.0.0.1:18080"), 1u);
    EXPECT_EQ(cfg.value().allowed_hosts.count("localhost:18080"), 1u);
    EXPECT_EQ(cfg.value().allowed_origins.count("http://127.0.0.1:18080"), 1u);
    EXPECT_EQ(cfg.value().allowed_origins.count("http://localhost:18080"), 1u);

    // Non-loopback must fail closed.
    const auto non_loopback = parse_server_config_json(R"({
        "format_version": "1.0",
        "http": { "listen_address": "0.0.0.0", "port": 8080 }
    })");
    EXPECT_FALSE(non_loopback.ok());

    // Duplicate key must fail closed.
    const auto dup = parse_server_config_json(R"({
        "format_version": "1.0",
        "format_version": "1.0"
    })");
    EXPECT_FALSE(dup.ok());

    // Unknown field must fail closed.
    const auto unknown = parse_server_config_json(R"({
        "format_version": "1.0",
        "extra": true
    })");
    EXPECT_FALSE(unknown.ok());

    // Shutdown timeout must exceed max ordinary timeout.
    const auto bad_timeout = parse_server_config_json(R"({
        "format_version": "1.0",
        "timeouts": {
            "mutation_acceptance": {"value": "10", "unit": "s"},
            "shutdown": {"value": "8", "unit": "s"}
        }
    })");
    EXPECT_FALSE(bad_timeout.ok());
}

TEST(StrictJsonCodecTest, EnforcesDuplicateKeysDepthLimitsUint64AndRfc3339) {
    const auto dup = parse_strict_json(R"({"a":1,"a":2})");
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(dup.error.code, "duplicate");
    EXPECT_EQ(dup.error.path, "/a");

    const auto deep = parse_strict_json("[[[[[1]]]]]", {1024, 3, 256, 64, 128});
    EXPECT_FALSE(deep.ok);
    EXPECT_EQ(deep.error.code, "too_large");

    // uint64 above JS MAX_SAFE_INTEGER (2^53 - 1) must parse exactly as decimal string.
    const auto big = parse_u64_decimal("18446744073709551615");
    ASSERT_TRUE(big.has_value());
    EXPECT_EQ(*big, 18446744073709551615ULL);
    EXPECT_FALSE(parse_u64_decimal("01").has_value());
    EXPECT_FALSE(parse_u64_decimal("-1").has_value());
    EXPECT_FALSE(parse_u64_decimal("18446744073709551616").has_value());

    // RFC 3339 UTC round-trip.
    const auto parsed_ts = parse_rfc3339_utc("2026-11-26T12:34:56.123456789Z");
    ASSERT_TRUE(parsed_ts.has_value());
    EXPECT_EQ(format_rfc3339_utc(*parsed_ts), "2026-11-26T12:34:56.123456789Z");
    EXPECT_FALSE(parse_rfc3339_utc("2026-11-26 12:34:56Z").has_value());
}

TEST(AuthStateTest, VerifiesPermissionsConstantTimeBearerSessionsAndTtl) {
    TempDir dir("auth");
    const std::string op_secret = "0123456789abcdef0123456789abcdef_operator_secret";
    const auto op_verifier = compute_bearer_verifier_hex(op_secret);
    ASSERT_TRUE(op_verifier.ok());
    ASSERT_EQ(op_verifier.value().size(), 64u);

    const fs::path cred_path = dir.path / "creds.json";
    const std::string creds_json = nlohmann::json{
        {"format_version", "1.0"},
        {"credentials",
         {{{"credential_id", "op-1"},
           {"principal", "operator"},
           {"scopes", {"read", "control"}},
           {"verifier", op_verifier.value()},
           {"credential_epoch", "1"},
           {"enabled", true}}}}}.dump();

    // Group-readable file (0640) must be rejected when enforce_file_permissions=true.
    write_file_mode(cred_path, creds_json, 0640);
    SecurityConfig sec;
    sec.credential_store = cred_path;
    sec.max_sessions = 2;
    sec.session_idle_timeout = 80ms;
    sec.session_absolute_timeout = 200ms;
    EXPECT_FALSE(AuthState::create(sec).ok());

    // Owner-only file (0600) succeeds.
    ASSERT_EQ(::chmod(cred_path.c_str(), 0600), 0);
    auto auth = AuthState::create(sec);
    ASSERT_TRUE(auth.ok()) << auth.status().message();

    // Valid bearer succeeds; wrong bearer fails and increments login_failures.
    const auto ok_bearer = auth.value()->authenticate_bearer(op_secret, {1, 0});
    ASSERT_TRUE(ok_bearer.ok());
    EXPECT_EQ(ok_bearer.value().principal, "operator");
    EXPECT_TRUE(ok_bearer.value().scopes.has(Scope::Read));
    EXPECT_TRUE(ok_bearer.value().scopes.has(Scope::Control));
    EXPECT_FALSE(ok_bearer.value().scopes.has(Scope::Admin));

    EXPECT_FALSE(
        auth.value()->authenticate_bearer("wrong_secret_with_at_least_32_bytes_here", {1, 0}).ok());
    EXPECT_EQ(auth.value()->login_failure_count(), 1u);

    // Session capacity is bounded (max_sessions = 2) and never evicts active sessions.
    auto s1 = auth.value()->create_session(op_secret, {1, 0});
    auto s2 = auth.value()->create_session(op_secret, {1, 0});
    ASSERT_TRUE(s1.ok());
    ASSERT_TRUE(s2.ok());
    const auto s3 = auth.value()->create_session(op_secret, {1, 0});
    EXPECT_FALSE(s3.ok());
    EXPECT_EQ(s3.status().code(), rodaq::StatusCode::ResourceExhausted);

    // GET session does not rotate CSRF token.
    const auto recovered = auth.value()->get_session(s1.value().session_id);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().csrf_token, s1.value().csrf_token);

    // Idle expiry frees slot for a new login.
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(auth.value()->authenticate_session(s1.value().session_id).ok());
    auto s4 = auth.value()->create_session(op_secret, {1, 0});
    ASSERT_TRUE(s4.ok());
    auth.value()->destroy_session(s4.value().session_id);
    EXPECT_FALSE(auth.value()->authenticate_session(s4.value().session_id).ok());
}

TEST(RangeAndDispositionTest, ParsesRfc9110SingleRangeIfRangeAndSanitizesFilename) {
    const auto full = parse_single_byte_range("", 1000);
    EXPECT_EQ(full.status, RangeParseStatus::NoRange);
    EXPECT_EQ(full.range.length, 1000u);
    EXPECT_FALSE(full.range.partial);

    const auto closed = parse_single_byte_range("bytes=10-29", 1000);
    EXPECT_EQ(closed.status, RangeParseStatus::Ok);
    EXPECT_EQ(closed.range.start, 10u);
    EXPECT_EQ(closed.range.end, 29u);
    EXPECT_EQ(closed.range.length, 20u);
    EXPECT_TRUE(closed.range.partial);

    const auto open = parse_single_byte_range("bytes=990-", 1000);
    EXPECT_EQ(open.status, RangeParseStatus::Ok);
    EXPECT_EQ(open.range.start, 990u);
    EXPECT_EQ(open.range.end, 999u);
    EXPECT_EQ(open.range.length, 10u);

    const auto suffix = parse_single_byte_range("bytes=-50", 1000);
    EXPECT_EQ(suffix.status, RangeParseStatus::Ok);
    EXPECT_EQ(suffix.range.start, 950u);
    EXPECT_EQ(suffix.range.end, 999u);
    EXPECT_EQ(suffix.range.length, 50u);

    EXPECT_EQ(
        parse_single_byte_range("bytes=0-10, 20-30", 1000).status,
        RangeParseStatus::MultipleRanges);
    EXPECT_EQ(
        parse_single_byte_range("bytes=1000-1005", 1000).status, RangeParseStatus::Unsatisfiable);
    EXPECT_EQ(parse_single_byte_range("bytes=20-10", 1000).status, RangeParseStatus::Malformed);

    EXPECT_TRUE(if_range_matches_etag("\"abc123\"", "abc123"));
    EXPECT_FALSE(if_range_matches_etag("W/\"abc123\"", "abc123"));
    EXPECT_FALSE(if_range_matches_etag("\"other\"", "abc123"));

    const auto disp = build_content_disposition("../../evil\r\nname/file.mcap");
    EXPECT_EQ(disp.find(".."), std::string::npos);
    EXPECT_EQ(disp.find('\r'), std::string::npos);
    EXPECT_EQ(disp.find('\n'), std::string::npos);
    EXPECT_NE(disp.find("attachment; filename=\""), std::string::npos);
}

TEST(ServiceCallExecutorTest, ReserveLaneSurvivesHungOrdinaryCallsAndTracksLateReturn) {
    ExecutionConfig cfg;
    cfg.control_workers = 1;
    cfg.control_reserve_workers = 1;
    cfg.control_queue = 4;
    cfg.control_reserve_queue = 2;
    auto exec = ServiceCallExecutor::create(cfg);
    ASSERT_TRUE(exec.ok());

    std::mutex mu;
    std::condition_variable cv;
    bool release_hung = false;
    bool hung_entered = false;
    std::atomic<int> hung_response_count{0};
    std::atomic<int> hung_timeout_count{0};

    // Submit a hung ordinary control call with a short 40ms observer timeout.
    const auto sub1 = exec.value()->submit(
        CallDomain::Control, AdmissionClass::Ordinary, "op1", 40ms,
        [&](const std::function<bool()>& try_claim) {
            {
                std::lock_guard<std::mutex> lk(mu);
                hung_entered = true;
            }
            cv.notify_all();
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return release_hung; });
            if (try_claim()) {
                hung_response_count.fetch_add(1);
            }
        },
        [&]() { hung_timeout_count.fetch_add(1); });
    ASSERT_TRUE(sub1.admitted);

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 2s, [&] { return hung_entered; }));
    }

    // While the ordinary control worker is blocked, a Stop on ControlReserve MUST still succeed!
    std::atomic<bool> reserve_done{false};
    const auto sub_stop = exec.value()->submit(
        CallDomain::Control, AdmissionClass::Reserve, "op1", 1s,
        [&](const std::function<bool()>& try_claim) {
            if (try_claim()) {
                reserve_done.store(true);
            }
        },
        [] {});
    ASSERT_TRUE(sub_stop.admitted);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!reserve_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(reserve_done.load());

    // Wait for the ordinary call's observer timeout to fire while worker is still active.
    while (hung_timeout_count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(hung_timeout_count.load(), 1);
    EXPECT_EQ(exec.value()->metrics().control_ordinary.active, 1u);

    // Release the hung Core call -> late completion recorded, response callback NOT invoked twice!
    {
        std::lock_guard<std::mutex> lk(mu);
        release_hung = true;
    }
    cv.notify_all();
    ASSERT_TRUE(exec.value()->shutdown(std::chrono::steady_clock::now() + 3s).ok());
    EXPECT_EQ(hung_response_count.load(), 0);
    EXPECT_EQ(hung_timeout_count.load(), 1);
    EXPECT_EQ(exec.value()->metrics().control_ordinary.late_completed, 1u);
}

TEST(ServiceCallExecutorTest, DistinguishesPrincipalQuotaFromGlobalSaturation) {
    ExecutionConfig cfg;
    cfg.control_workers = 1;
    cfg.control_queue = 6;  // principal cap = max(4, 6/2) = 4, global queue = 6
    cfg.control_reserve_workers = 1;
    cfg.control_reserve_queue = 2;
    auto exec = ServiceCallExecutor::create(cfg);
    ASSERT_TRUE(exec.ok());

    std::mutex mu;
    std::condition_variable cv;
    bool worker_entered = false;
    bool release = false;

    // 1st call occupies worker + counts toward principal "alice".
    auto s1 = exec.value()->submit(
        CallDomain::Control, AdmissionClass::Ordinary, "alice", 2s,
        [&](const std::function<bool()>&) {
            {
                std::lock_guard<std::mutex> lk(mu);
                worker_entered = true;
            }
            cv.notify_all();
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return release; });
        },
        [] {});
    ASSERT_TRUE(s1.admitted);
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 2s, [&] { return worker_entered; }));
    }

    // 3 more queued calls for "alice" fill her per-principal quota (4).
    for (int i = 0; i < 3; ++i) {
        auto r = exec.value()->submit(
            CallDomain::Control, AdmissionClass::Ordinary, "alice", 2s,
            [](const std::function<bool()>&) {}, [] {});
        ASSERT_TRUE(r.admitted);
    }
    // 5th call for "alice" hits PrincipalQuota (HTTP 429).
    auto alice_rej = exec.value()->submit(
        CallDomain::Control, AdmissionClass::Ordinary, "alice", 2s,
        [](const std::function<bool()>&) {}, [] {});
    EXPECT_FALSE(alice_rej.admitted);
    EXPECT_EQ(alice_rej.failure, CallAdmissionFailure::PrincipalQuota);

    // Meanwhile "bob" can still enqueue 3 calls until global queue (6) saturates ->
    // GlobalSaturation (HTTP 503).
    for (int i = 0; i < 3; ++i) {
        auto r = exec.value()->submit(
            CallDomain::Control, AdmissionClass::Ordinary, "bob", 2s,
            [](const std::function<bool()>&) {}, [] {});
        ASSERT_TRUE(r.admitted);
    }
    auto bob_rej = exec.value()->submit(
        CallDomain::Control, AdmissionClass::Ordinary, "bob", 2s,
        [](const std::function<bool()>&) {}, [] {});
    EXPECT_FALSE(bob_rej.admitted);
    EXPECT_EQ(bob_rej.failure, CallAdmissionFailure::GlobalSaturation);

    const auto m = exec.value()->metrics().control_ordinary;
    EXPECT_EQ(m.rejected_principal, 1u);
    EXPECT_EQ(m.rejected_global, 1u);

    {
        std::lock_guard<std::mutex> lk(mu);
        release = true;
    }
    cv.notify_all();
    ASSERT_TRUE(exec.value()->shutdown(std::chrono::steady_clock::now() + 3s).ok());
}

TEST(DownloadRegistryTest, EnforcesPrincipalAndGlobalLimitsAndCleansUpOnShutdown) {
    DownloadConfig cfg;
    cfg.workers = 2;
    cfg.max_concurrent = 4;  // per_principal = max(2, 4/2) = 2
    cfg.chunk_bytes = 65536;
    cfg.max_bytes_in_flight = 131072;
    auto reg = DownloadRegistry::create(cfg);
    ASSERT_TRUE(reg.ok());

    ByteRange r{0, 99, 100, false};
    auto a1 = reg.value()->create_session("alice", r, nullptr, {});
    auto a2 = reg.value()->create_session("alice", r, nullptr, {});
    ASSERT_TRUE(a1.admitted);
    ASSERT_TRUE(a2.admitted);
    // 3rd download for alice rejected by principal quota (429).
    auto a3 = reg.value()->create_session("alice", r, nullptr, {});
    EXPECT_FALSE(a3.admitted);
    EXPECT_EQ(a3.failure, DownloadAdmissionFailure::PrincipalQuota);

    // bob takes remaining 2 global slots; charlie rejected by global saturation (503).
    auto b1 = reg.value()->create_session("bob", r, nullptr, {});
    auto b2 = reg.value()->create_session("bob", r, nullptr, {});
    ASSERT_TRUE(b1.admitted);
    ASSERT_TRUE(b2.admitted);
    auto c1 = reg.value()->create_session("charlie", r, nullptr, {});
    EXPECT_FALSE(c1.admitted);
    EXPECT_EQ(c1.failure, DownloadAdmissionFailure::GlobalSaturation);

    EXPECT_EQ(reg.value()->active_count(), 4u);
    ASSERT_TRUE(reg.value()->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(reg.value()->active_count(), 0u);
    const auto dm = reg.value()->metrics();
    EXPECT_EQ(dm.rejected_principal, 1u);
    EXPECT_EQ(dm.rejected_global, 1u);
    EXPECT_EQ(dm.total_cancelled, 4u);
}

TEST(DownloadRegistryTest, NoexceptIfRangeAndDiskReadAdmissionFailureSettleOnce) {
    // IC-SRV-01: verify zero-allocation noexcept If-Range comparison.
    static_assert(noexcept(if_range_matches_etag("", "")));
    EXPECT_TRUE(if_range_matches_etag("", "abc"));
    EXPECT_TRUE(if_range_matches_etag("\"abc\"", "abc"));
    EXPECT_TRUE(if_range_matches_etag("  \"abc\"  ", "\"abc\""));
    EXPECT_FALSE(if_range_matches_etag("W/\"abc\"", "abc"));
    EXPECT_FALSE(if_range_matches_etag("\"xyz\"", "abc"));

    // IC-SRV-01: disk-read admission returning false or throwing std::bad_alloc
    // rolls back inflight_read_, settles strictly once, and never terminates.
    for (const bool throw_alloc : {false, true}) {
        std::atomic<int> terminal_calls{0};
        std::atomic<bool> was_cancelled{false};
        DownloadSession::Options opts;
        opts.session_id = throw_alloc ? 2u : 1u;
        opts.principal = "alice";
        opts.chunk_bytes = 4096;
        opts.max_bytes_in_flight = 8192;
        opts.range = {0, 99, 100, false};
        opts.submit_disk_read =
            [throw_alloc](std::shared_ptr<DownloadSession>, std::uint64_t, std::size_t) -> bool {
            if (throw_alloc) {
                throw std::bad_alloc();
            }
            return false;
        };
        opts.on_terminal = [&](std::uint64_t, std::uint64_t, bool cancelled) {
            terminal_calls.fetch_add(1);
            was_cancelled.store(cancelled);
        };
        auto session = DownloadSession::create(std::move(opts));
        ASSERT_NE(session, nullptr);
        // Calling on_transport_writable() / attach_and_start() / request_cancel()
        // must never terminate and must settle strictly once.
        session->attach_and_start(nullptr, "");
        EXPECT_TRUE(session->is_settled());
        EXPECT_EQ(terminal_calls.load(), 1);
        session->on_transport_writable();
        session->request_cancel();
        EXPECT_EQ(terminal_calls.load(), 1);
    }

    // Exercise noexcept cancel_all() and notify_connection_closed() directly.
    DownloadConfig cfg;
    cfg.workers = 1;
    cfg.max_concurrent = 4;
    cfg.chunk_bytes = 4096;
    cfg.max_bytes_in_flight = 8192;
    auto reg = DownloadRegistry::create(cfg);
    ASSERT_TRUE(reg.ok());
    static_assert(noexcept(reg.value()->cancel_all()));
    static_assert(noexcept(reg.value()->notify_connection_closed(nullptr)));
    ByteRange r{0, 63, 64, false};
    ASSERT_TRUE(reg.value()->create_session("alice", r, nullptr, {}).admitted);
    ASSERT_TRUE(reg.value()->create_session("bob", r, nullptr, {}).admitted);
    reg.value()->cancel_all();
    EXPECT_EQ(reg.value()->active_count(), 0u);
    ASSERT_TRUE(reg.value()->shutdown(std::chrono::steady_clock::now() + 2s).ok());
}

TEST(StaticAssetServiceTest, EnforcesManifestMajorCachePolicyAndNeverFallbacksApi) {
    TempDir dir("web");
    write_file_mode(dir.path / "index.html", "<html><body>RoDAQ</body></html>", 0644);
    write_file_mode(dir.path / "assets/app.abc123.js", "console.log('ok');", 0644);
    write_file_mode(
        dir.path / "web_manifest.json",
        R"({"web_build_id":"build-1","api_major":1,"api_minor_min":0})", 0644);

    const auto svc = StaticAssetService::create(dir.path, 1);
    EXPECT_TRUE(svc.usable());
    EXPECT_FALSE(svc.deployment_error().has_value());

    const auto root = svc.resolve("/");
    ASSERT_TRUE(root.found);
    EXPECT_EQ(root.cache_control, "no-cache");

    const auto asset = svc.resolve("/assets/app.abc123.js");
    ASSERT_TRUE(asset.found);
    EXPECT_EQ(asset.cache_control, "public, max-age=31536000, immutable");

    const auto spa = svc.resolve("/tasks/123");
    ASSERT_TRUE(spa.found);
    EXPECT_TRUE(spa.is_spa_fallback);

    EXPECT_FALSE(svc.resolve("/api/unknown").found);
    EXPECT_FALSE(svc.resolve("/../etc/passwd").found);

    // Incompatible major fails deployment check cleanly.
    const auto bad = StaticAssetService::create(dir.path, 2);
    EXPECT_FALSE(bad.usable());
    ASSERT_TRUE(bad.deployment_error().has_value());
    EXPECT_EQ(*bad.deployment_error(), "web_bundle_api_major_mismatch");
}

}  // namespace
