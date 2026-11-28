#include "api/dto.hpp"
#include "api/error_response.hpp"
#include "api/json_codec.hpp"
#include "config.hpp"
#include "download/download_registry.hpp"
#include "download/download_session.hpp"
#include "security/auth.hpp"
#include "server_app.hpp"
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
        "http": { "listen_address": "127.0.0.1", "port": 18080, "recommended_polling_floor_ms": 500 },
        "execution": { "control_workers": 2, "control_queue": 32 },
        "security": { "bearer_token_file": "/tmp/rodaq_server.token" },
        "timeouts": { "query_ms": 2000, "mutation_acceptance_ms": 4000, "shutdown_ms": 15000 }
    })";
    auto cfg = parse_server_config_json(valid_json);
    ASSERT_TRUE(cfg.ok()) << cfg.status().message();
    EXPECT_EQ(cfg.value().http.listen_address, "127.0.0.1");
    EXPECT_EQ(cfg.value().http.port, 18080);
    EXPECT_EQ(cfg.value().allowed_hosts.count("127.0.0.1:18080"), 1u);
    EXPECT_EQ(cfg.value().allowed_hosts.count("localhost:18080"), 1u);
    EXPECT_EQ(cfg.value().allowed_origins.count("http://127.0.0.1:18080"), 1u);
    EXPECT_EQ(cfg.value().allowed_origins.count("http://localhost:18080"), 1u);

    const auto non_loopback = parse_server_config_json(R"({
        "format_version": "1.0",
        "http": { "listen_address": "0.0.0.0", "port": 8080 }
    })");
    EXPECT_FALSE(non_loopback.ok());

    const auto unknown = parse_server_config_json(R"({
        "format_version": "1.0",
        "extra": true
    })");
    EXPECT_FALSE(unknown.ok());

    const auto bad_timeout = parse_server_config_json(R"({
        "format_version": "1.0",
        "timeouts": {
            "mutation_acceptance_ms": 10000,
            "shutdown_ms": 8000
        }
    })");
    EXPECT_FALSE(bad_timeout.ok());
}

TEST(BoundedJsonCodecTest, EnforcesSizeDepthUint64AndRfc3339) {
    const auto deep = parse_bounded_json("[[[[[1]]]]]", {1024, 3});
    EXPECT_FALSE(deep.ok);
    EXPECT_EQ(deep.error.code, "too_large");

    const auto big = parse_u64_decimal("18446744073709551615");
    ASSERT_TRUE(big.has_value());
    EXPECT_EQ(*big, 18446744073709551615ULL);
    EXPECT_FALSE(parse_u64_decimal("01").has_value());
    EXPECT_FALSE(parse_u64_decimal("-1").has_value());
    EXPECT_FALSE(parse_u64_decimal("18446744073709551616").has_value());

    const auto parsed_ts = parse_rfc3339_utc("2026-11-27T12:34:56.123456789Z");
    ASSERT_TRUE(parsed_ts.has_value());
    EXPECT_EQ(format_rfc3339_utc(*parsed_ts), "2026-11-27T12:34:56.123456789Z");
    EXPECT_FALSE(parse_rfc3339_utc("2026-11-27 12:34:56Z").has_value());
}

TEST(BearerAuthenticatorTest, EnforcesFilePermissionsAndConstantTimeMatch) {
    TempDir dir("auth");
    const std::string secret = "0123456789abcdef0123456789abcdef_local_token";
    const fs::path token_path = dir.path / "server.token";

    // Group-readable file (0640) must be rejected when enforce_file_permissions=true.
    write_file_mode(token_path, secret + "\n", 0640);
    SecurityConfig sec;
    sec.bearer_token_file = token_path;
    sec.enforce_file_permissions = true;
    EXPECT_FALSE(BearerAuthenticator::create(sec).ok());

    // Owner-only file (0600) succeeds.
    ASSERT_EQ(::chmod(token_path.c_str(), 0600), 0);
    auto auth = BearerAuthenticator::create(sec);
    ASSERT_TRUE(auth.ok()) << auth.status().message();

    EXPECT_TRUE(auth.value()->authenticate(secret));
    EXPECT_FALSE(auth.value()->authenticate("wrong_secret_with_at_least_32_bytes_here"));
    EXPECT_FALSE(auth.value()->authenticate("too-short"));
    EXPECT_EQ(auth.value()->failure_count(), 2u);
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

TEST(ControlExecutorTest, ObserverTimeoutRetainsWorkerSlotUntilLateReturnAndEnforcesQueueBound) {
    ExecutionConfig cfg;
    cfg.control_workers = 1;
    cfg.control_queue = 2;
    auto exec = ControlExecutor::create(cfg);
    ASSERT_TRUE(exec.ok());

    std::mutex mu;
    std::condition_variable cv;
    bool release_hung = false;
    bool hung_entered = false;
    std::atomic<int> hung_response_count{0};
    std::atomic<int> hung_timeout_count{0};

    const auto sub1 = exec.value()->submit(
        40ms,
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

    // Fill queue (2 slots), 3rd queued call must reject with GlobalSaturation (503).
    ASSERT_TRUE(exec.value()
                    ->submit(
                        2s, [](const std::function<bool()>&) {}, [] {})
                    .admitted);
    ASSERT_TRUE(exec.value()
                    ->submit(
                        2s, [](const std::function<bool()>&) {}, [] {})
                    .admitted);
    const auto sat = exec.value()->submit(
        2s, [](const std::function<bool()>&) {}, [] {});
    EXPECT_FALSE(sat.admitted);
    EXPECT_EQ(sat.failure, CallAdmissionFailure::GlobalSaturation);

    // Wait for observer timeout while hung Core call still owns worker slot.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (hung_timeout_count.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(hung_timeout_count.load(), 1);
    EXPECT_EQ(exec.value()->metrics().active, 1u);

    {
        std::lock_guard<std::mutex> lk(mu);
        release_hung = true;
    }
    cv.notify_all();
    ASSERT_TRUE(exec.value()->shutdown(std::chrono::steady_clock::now() + 3s).ok());
    EXPECT_EQ(hung_response_count.load(), 0);
    EXPECT_EQ(hung_timeout_count.load(), 1);
    const auto m = exec.value()->metrics();
    EXPECT_EQ(m.rejected, 1u);
    EXPECT_EQ(m.late_completed, 1u);
}

TEST(
    ControlExecutorTest, ShutdownTimeoutDetachesWorkerWithoutUafWhenOwnerDestroysBeforeLateReturn) {
    struct SharedBacking {
        std::mutex mu;
        std::condition_variable cv;
        bool entered{false};
        bool release{false};
        bool done{false};
        std::uint64_t magic{0x524f444151ULL};
    };
    auto backing = std::make_shared<SharedBacking>();

    ExecutionConfig cfg;
    cfg.control_workers = 1;
    cfg.control_queue = 2;
    auto exec = ControlExecutor::create(cfg);
    ASSERT_TRUE(exec.ok());

    const auto sub = exec.value()->submit(
        5s,
        [backing](const std::function<bool()>& try_claim) {
            {
                std::lock_guard<std::mutex> lk(backing->mu);
                backing->entered = true;
            }
            backing->cv.notify_all();
            std::unique_lock<std::mutex> lk(backing->mu);
            backing->cv.wait(lk, [&] { return backing->release; });
            // Access shared backing after ControlExecutor owner has been destroyed.
            EXPECT_EQ(backing->magic, 0x524f444151ULL);
            (void)try_claim();
            backing->done = true;
            backing->cv.notify_all();
        },
        [] {});
    ASSERT_TRUE(sub.admitted);

    {
        std::unique_lock<std::mutex> lk(backing->mu);
        ASSERT_TRUE(backing->cv.wait_for(lk, 2s, [&] { return backing->entered; }));
    }

    const auto sd = exec.value()->shutdown(std::chrono::steady_clock::now() + 20ms);
    EXPECT_EQ(sd.status().code(), rodaq::StatusCode::DeadlineExceeded);

    // Destroy owner completely while detached worker is still blocked.
    exec.value().reset();

    {
        std::lock_guard<std::mutex> lk(backing->mu);
        backing->release = true;
    }
    backing->cv.notify_all();

    {
        std::unique_lock<std::mutex> lk(backing->mu);
        ASSERT_TRUE(backing->cv.wait_for(lk, 2s, [&] { return backing->done; }));
    }
}

TEST(
    DownloadRegistryTest,
    ShutdownTimeoutDetachesOpenWorkerWithoutUafWhenOwnerDestroysBeforeLateReturn) {
    struct SharedBacking {
        std::mutex mu;
        std::condition_variable cv;
        bool entered{false};
        bool release{false};
        bool done{false};
        std::uint64_t magic{0x444f574e4cULL};
    };
    auto backing = std::make_shared<SharedBacking>();

    DownloadConfig cfg;
    cfg.workers = 1;
    cfg.max_concurrent = 2;
    cfg.chunk_bytes = 65536;
    cfg.max_bytes_in_flight = 131072;
    auto reg = DownloadRegistry::create(cfg);
    ASSERT_TRUE(reg.ok());

    ASSERT_TRUE(reg.value()->submit_open([backing]() {
        {
            std::lock_guard<std::mutex> lk(backing->mu);
            backing->entered = true;
        }
        backing->cv.notify_all();
        std::unique_lock<std::mutex> lk(backing->mu);
        backing->cv.wait(lk, [&] { return backing->release; });
        EXPECT_EQ(backing->magic, 0x444f574e4cULL);
        backing->done = true;
        backing->cv.notify_all();
    }));

    {
        std::unique_lock<std::mutex> lk(backing->mu);
        ASSERT_TRUE(backing->cv.wait_for(lk, 2s, [&] { return backing->entered; }));
    }

    const auto sd = reg.value()->shutdown(std::chrono::steady_clock::now() + 20ms);
    EXPECT_EQ(sd.status().code(), rodaq::StatusCode::DeadlineExceeded);

    // Destroy owner completely while detached open worker is still blocked.
    reg.value().reset();

    {
        std::lock_guard<std::mutex> lk(backing->mu);
        backing->release = true;
    }
    backing->cv.notify_all();

    {
        std::unique_lock<std::mutex> lk(backing->mu);
        ASSERT_TRUE(backing->cv.wait_for(lk, 2s, [&] { return backing->done; }));
    }
}

TEST(DownloadRegistryTest, EnforcesGlobalLimitNoexceptCancelAndShutdownCleanup) {
    DownloadConfig cfg;
    cfg.workers = 2;
    cfg.max_concurrent = 3;
    cfg.chunk_bytes = 65536;
    cfg.max_bytes_in_flight = 131072;
    auto reg = DownloadRegistry::create(cfg);
    ASSERT_TRUE(reg.ok());

    ByteRange r{0, 99, 100, false};
    ASSERT_TRUE(reg.value()->create_session(r, nullptr, {}).admitted);
    ASSERT_TRUE(reg.value()->create_session(r, nullptr, {}).admitted);
    ASSERT_TRUE(reg.value()->create_session(r, nullptr, {}).admitted);
    const auto sat = reg.value()->create_session(r, nullptr, {});
    EXPECT_FALSE(sat.admitted);
    EXPECT_EQ(sat.failure, DownloadAdmissionFailure::GlobalSaturation);

    EXPECT_EQ(reg.value()->active_count(), 3u);
    ASSERT_TRUE(reg.value()->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(reg.value()->active_count(), 0u);
    const auto dm = reg.value()->metrics();
    EXPECT_EQ(dm.rejected_global, 1u);
    EXPECT_EQ(dm.total_cancelled, 3u);
}

TEST(DownloadRegistryTest, NoexceptIfRangeAndDiskReadAdmissionFailureSettleOnce) {
    static_assert(noexcept(if_range_matches_etag("", "")));
    EXPECT_TRUE(if_range_matches_etag("", "abc"));
    EXPECT_TRUE(if_range_matches_etag("\"abc\"", "abc"));
    EXPECT_TRUE(if_range_matches_etag("  \"abc\"  ", "\"abc\""));
    EXPECT_FALSE(if_range_matches_etag("W/\"abc\"", "abc"));
    EXPECT_FALSE(if_range_matches_etag("\"xyz\"", "abc"));

    for (const bool throw_alloc : {false, true}) {
        std::atomic<int> terminal_calls{0};
        DownloadSession::Options opts;
        opts.session_id = throw_alloc ? 2u : 1u;
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
        opts.on_terminal = [&](std::uint64_t, std::uint64_t, bool) { terminal_calls.fetch_add(1); };
        auto session = DownloadSession::create(std::move(opts));
        ASSERT_NE(session, nullptr);
        session->attach_and_start(nullptr, "");
        EXPECT_TRUE(session->is_settled());
        EXPECT_EQ(terminal_calls.load(), 1);
        session->on_transport_writable();
        session->request_cancel();
        EXPECT_EQ(terminal_calls.load(), 1);
    }

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
    ASSERT_TRUE(reg.value()->create_session(r, nullptr, {}).admitted);
    ASSERT_TRUE(reg.value()->create_session(r, nullptr, {}).admitted);
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

    const auto bad = StaticAssetService::create(dir.path, 2);
    EXPECT_FALSE(bad.usable());
    ASSERT_TRUE(bad.deployment_error().has_value());
    EXPECT_EQ(*bad.deployment_error(), "web_bundle_api_major_mismatch");
}

TEST(ControlExecutorTest, PreAdmissionFaultNeverRunsMutationOrDoubleCallsCallback) {
    ExecutionConfig cfg;
    cfg.control_workers = 1;
    cfg.control_queue = 2;
    auto exec = ControlExecutor::create(cfg);
    ASSERT_TRUE(exec.ok());

    for (const auto fault_point :
         {ControlExecutor::SubmitFaultPoint::TimerRegister,
          ControlExecutor::SubmitFaultPoint::QueuePush}) {
        std::atomic<int> mutation_calls{0};
        std::atomic<int> response_calls{0};
        exec.value()->set_submit_fault_for_test(fault_point);
        const auto sub = exec.value()->submit(
            10ms,
            [&](const std::function<bool()>& try_claim) {
                mutation_calls.fetch_add(1, std::memory_order_acq_rel);
                if (try_claim()) {
                    response_calls.fetch_add(1, std::memory_order_acq_rel);
                }
            },
            [&]() { response_calls.fetch_add(1, std::memory_order_acq_rel); });
        EXPECT_FALSE(sub.admitted);
        EXPECT_EQ(sub.failure, CallAdmissionFailure::GlobalSaturation);
        // Simulate caller sending 503 synchronously on rejection.
        response_calls.fetch_add(1, std::memory_order_acq_rel);

        // Wait past the observer timeout so any leaked timer would have fired.
        std::this_thread::sleep_for(40ms);
        EXPECT_EQ(mutation_calls.load(std::memory_order_acquire), 0);
        EXPECT_EQ(response_calls.load(std::memory_order_acquire), 1);
    }

    // Subsequent ordinary submission must still succeed cleanly after rollback.
    std::atomic<int> ok_calls{0};
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    const auto sub = exec.value()->submit(
        2s,
        [&](const std::function<bool()>& try_claim) {
            if (try_claim()) {
                ok_calls.fetch_add(1, std::memory_order_acq_rel);
            }
            std::lock_guard<std::mutex> lk(mu);
            done = true;
            cv.notify_all();
        },
        [&]() { ok_calls.fetch_add(10, std::memory_order_acq_rel); });
    ASSERT_TRUE(sub.admitted);
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 2s, [&] { return done; }));
    }
    EXPECT_EQ(ok_calls.load(std::memory_order_acquire), 1);
    ASSERT_TRUE(exec.value()->shutdown(std::chrono::steady_clock::now() + 2s).ok());
}

TEST(ServerAppTest, SharedStartupLatchSurvivesLateAdviceAfterWaiterExitAndQuiescenceStartsTrue) {
    // Verify a late BeginningAdvice callback after wait_running_for timeout never
    // touches a destroyed stack frame (verified under ASan).
    std::function<void()> late_advice;
    {
        auto latch = std::make_shared<ServerStartupLatch>();
        late_advice = [latch]() {
            latch->mark_running();
            latch->mark_exited();
        };
        EXPECT_FALSE(latch->wait_running_for(5ms));
    }
    // Original waiter scope has exited; invoking the late advice must be safe.
    ASSERT_TRUE(static_cast<bool>(late_advice));
    late_advice();

    // Unstarted / failed-validation ServerApp remains quiescent so caller may
    // safely tear down Core without tripping fail-closed _Exit.
    ServerApp app;
    EXPECT_TRUE(app.quiescent());
    ServerConfig cfg;
    ServerServices empty_services;
    const auto res = app.start(cfg, empty_services);
    EXPECT_FALSE(res.ok());
    EXPECT_TRUE(app.quiescent());
    EXPECT_TRUE(app.shutdown(std::chrono::steady_clock::now() + 1s).ok());
    EXPECT_TRUE(app.quiescent());
}

}  // namespace
