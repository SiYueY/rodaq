#include "api/dto.hpp"
#include "download/download_session.hpp"
#include "security/auth.hpp"
#include "server_app.hpp"
#include "server_config.hpp"
#include "service_call_executor.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace srv = rodaq::server;
using namespace std::chrono_literals;
using rodaq::StatusCode;

class ServerContractTest : public testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/rodaq-s5-XXXXXX";
        const auto* dir = ::mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        root_ = dir;
        cred_path_ = root_ / "server_credentials.json";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    void write_credentials(const std::string& content, mode_t mode = 0600) {
        std::ofstream out(cred_path_, std::ios::binary | std::ios::trunc);
        out << content;
        out.close();
        ASSERT_EQ(::chmod(cred_path_.c_str(), mode), 0);
    }

    fs::path root_;
    fs::path cred_path_;
};

TEST_F(
    ServerContractTest, ServerConfigStrictParserValidatesLoopbackBoundsAndRejectsUnknownOrDupes) {
    const std::string valid_json =
        std::string(
            R"({"format_version":"1.0","http":{"listen_address":"127.0.0.1","port":18080},)") +
        R"("security":{"credential_store":")" + cred_path_.string() + R"("}})";
    auto cfg = srv::parse_server_config(valid_json);
    ASSERT_TRUE(cfg.ok()) << cfg.status().message();
    EXPECT_EQ(cfg.value().http.listen_address, "127.0.0.1");
    EXPECT_EQ(cfg.value().http.port, 18080);
    EXPECT_NE(
        std::find(
            cfg.value().allowed_hosts.begin(), cfg.value().allowed_hosts.end(), "127.0.0.1:18080"),
        cfg.value().allowed_hosts.end());
    EXPECT_NE(
        std::find(
            cfg.value().allowed_origins.begin(), cfg.value().allowed_origins.end(),
            "http://localhost:18080"),
        cfg.value().allowed_origins.end());

    // Non-loopback MUST fail-fast.
    EXPECT_FALSE(srv::parse_server_config(
                     R"({"format_version":"1.0","http":{"listen_address":"0.0.0.0","port":8080}})")
                     .ok());
    // Unknown field MUST fail-fast.
    EXPECT_FALSE(srv::parse_server_config(R"({"format_version":"1.0","unknown_section":{}})").ok());
    // Duplicate key MUST fail-fast.
    EXPECT_FALSE(
        srv::parse_server_config(R"({"format_version":"1.0","format_version":"1.0"})").ok());
    // Shutdown timeout <= observer timeout MUST fail-fast.
    EXPECT_FALSE(
        srv::parse_server_config(
            R"({"format_version":"1.0","timeouts":{"mutation_acceptance":{"value":"6","unit":"s"},"shutdown":{"value":"5","unit":"s"}}})")
            .ok());
}

TEST_F(ServerContractTest, AuthStateEnforcesPermissionsVerifierSessionsAndCsrf) {
    const std::string op_bearer = "0123456789abcdef0123456789abcdef_operator_secret";
    const std::string admin_bearer = "fedcba9876543210fedcba9876543210_admin_secret_99";
    const auto op_verifier = srv::compute_bearer_verifier(op_bearer);
    const auto admin_verifier = srv::compute_bearer_verifier(admin_bearer);
    ASSERT_TRUE(op_verifier.ok());
    ASSERT_TRUE(admin_verifier.ok());
    EXPECT_EQ(op_verifier.value().size(), 64u);

    const std::string cred_json = nlohmann::json{
        {"format_version", "1.0"},
        {"credentials",
         {{{"credential_id", "op1"},
           {"principal", "operator"},
           {"scopes", {"read", "control"}},
           {"verifier", op_verifier.value()},
           {"credential_epoch", "1"},
           {"enabled", true}},
          {{"credential_id", "adm1"},
           {"principal", "admin"},
           {"scopes", {"read", "control", "admin"}},
           {"verifier", admin_verifier.value()},
           {"credential_epoch", "1"},
           {"enabled", true}}}}}.dump();

    // Overly broad mode (0644) MUST be rejected when enforce_credential_file_mode=true.
    write_credentials(cred_json, 0644);
    srv::SecurityConfig sec;
    sec.credential_store = cred_path_;
    sec.max_sessions = 1;
    sec.session_idle_timeout = 5s;
    sec.session_absolute_timeout = 10s;
    EXPECT_EQ(srv::AuthState::create(sec).status().code(), StatusCode::PermissionDenied);

    // Mode 0600 succeeds.
    write_credentials(cred_json, 0600);
    auto auth = srv::AuthState::create(sec);
    ASSERT_TRUE(auth.ok()) << auth.status().message();

    // Bearer authentication verifies scopes and rejects invalid bearer.
    auto b_ok = auth.value()->authenticate_bearer(op_bearer, {1, 0});
    ASSERT_TRUE(b_ok.ok());
    EXPECT_EQ(b_ok.value().principal, "operator");
    EXPECT_TRUE(b_ok.value().scopes.has(srv::Scope::Read));
    EXPECT_TRUE(b_ok.value().scopes.has(srv::Scope::Control));
    EXPECT_FALSE(b_ok.value().scopes.has(srv::Scope::Admin));
    EXPECT_FALSE(
        auth.value()->authenticate_bearer("wrong_bearer_0123456789abcdef0123456789", {1, 0}).ok());

    // Session creation enforces max_sessions=1 without evicting active session.
    auto s1 = auth.value()->create_session(op_bearer, {1, 0});
    ASSERT_TRUE(s1.ok());
    EXPECT_EQ(auth.value()->active_sessions(), 1u);
    auto s2 = auth.value()->create_session(admin_bearer, {1, 0});
    EXPECT_EQ(s2.status().code(), StatusCode::ResourceExhausted);

    // Session cookie auth + CSRF verification.
    auto ctx = auth.value()->authenticate_session(s1.value().session_id);
    ASSERT_TRUE(ctx.ok());
    EXPECT_TRUE(ctx.value().via_session_cookie);
    EXPECT_TRUE(auth.value()->verify_csrf(ctx.value(), s1.value().csrf_token));
    EXPECT_FALSE(auth.value()->verify_csrf(ctx.value(), "forged-csrf-token"));

    // Logout frees session slot immediately.
    auth.value()->destroy_session(s1.value().session_id);
    EXPECT_EQ(auth.value()->active_sessions(), 0u);
    EXPECT_FALSE(auth.value()->authenticate_session(s1.value().session_id).ok());
}

TEST_F(ServerContractTest, ExecutorIsolatesReserveLaneAndTracksLateReturnAfterTimeout) {
    srv::ExecutionConfig cfg;
    cfg.control_workers = 1;
    cfg.control_reserve_workers = 1;
    cfg.control_queue = 4;
    cfg.control_reserve_queue = 2;
    cfg.query_workers = 1;
    cfg.query_reserve_workers = 1;
    cfg.query_queue = 4;
    cfg.query_reserve_queue = 2;
    cfg.maintenance_workers = 1;
    cfg.maintenance_queue = 4;

    auto exec = srv::ServiceCallExecutor::create(cfg);
    ASSERT_TRUE(exec.ok());

    std::atomic<bool> release_hung{false};
    std::atomic<bool> timeout_fired{false};
    std::atomic<bool> late_claim_result{true};

    // Submit a hung ordinary control call with short observer timeout (40ms).
    srv::RequestContext hung_ctx;
    hung_ctx.request_id = "req-hung";
    hung_ctx.observer_deadline = std::chrono::steady_clock::now() + 40ms;
    ASSERT_TRUE(exec.value()
                    ->submit(
                        srv::CallDomain::Control, srv::AdmissionClass::Ordinary, hung_ctx,
                        [&](const auto& claim) {
                            while (!release_hung.load(std::memory_order_acquire)) {
                                std::this_thread::sleep_for(5ms);
                            }
                            late_claim_result.store(claim(), std::memory_order_release);
                        },
                        [&]() { timeout_fired.store(true, std::memory_order_release); })
                    .ok());

    // While ordinary control worker is hung, a Control Reserve (Stop) call MUST still execute!
    std::atomic<bool> reserve_ran{false};
    srv::RequestContext res_ctx;
    res_ctx.request_id = "req-stop-reserve";
    res_ctx.observer_deadline = std::chrono::steady_clock::now() + 2s;
    ASSERT_TRUE(exec.value()
                    ->submit(
                        srv::CallDomain::Control, srv::AdmissionClass::Reserve, res_ctx,
                        [&](const auto& claim) {
                            if (claim()) {
                                reserve_ran.store(true, std::memory_order_release);
                            }
                        },
                        []() {})
                    .ok());

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while ((!reserve_ran.load() || !timeout_fired.load()) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(reserve_ran.load());
    EXPECT_TRUE(timeout_fired.load());
    // Active slot MUST still be held while hung call has not genuinely returned!
    EXPECT_GE(exec.value()->metrics().control_active, 1u);

    // Now let the hung call return: claim() must return false and late_completed increments.
    release_hung.store(true, std::memory_order_release);
    const auto settle_deadline = std::chrono::steady_clock::now() + 1s;
    while (exec.value()->metrics().late_completed == 0 &&
           std::chrono::steady_clock::now() < settle_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_FALSE(late_claim_result.load());
    EXPECT_EQ(exec.value()->metrics().late_completed, 1u);
    ASSERT_TRUE(exec.value()->shutdown(std::chrono::steady_clock::now() + 2s).ok());
}

TEST_F(ServerContractTest, ByteRangeParserAndFilenameSanitizerMatchContract) {
    const auto r1 = srv::parse_single_byte_range("bytes=0-99", 500);
    EXPECT_EQ(r1.outcome, srv::RangeParseOutcome::Valid);
    EXPECT_EQ(r1.range.start, 0u);
    EXPECT_EQ(r1.range.end, 99u);

    const auto r2 = srv::parse_single_byte_range("bytes=400-", 500);
    EXPECT_EQ(r2.outcome, srv::RangeParseOutcome::Valid);
    EXPECT_EQ(r2.range.start, 400u);
    EXPECT_EQ(r2.range.end, 499u);

    const auto r3 = srv::parse_single_byte_range("bytes=-50", 500);
    EXPECT_EQ(r3.outcome, srv::RangeParseOutcome::Valid);
    EXPECT_EQ(r3.range.start, 450u);
    EXPECT_EQ(r3.range.end, 499u);

    EXPECT_EQ(
        srv::parse_single_byte_range("bytes=0-10, 20-30", 500).outcome,
        srv::RangeParseOutcome::MultipleNotSupported);
    EXPECT_EQ(
        srv::parse_single_byte_range("bytes=600-700", 500).outcome,
        srv::RangeParseOutcome::Unsatisfiable);
    EXPECT_EQ(
        srv::parse_single_byte_range("bytes=50-10", 500).outcome,
        srv::RangeParseOutcome::Malformed);

    EXPECT_EQ(srv::sanitize_display_filename("../bad\r\n/name;\"x.mcap"), ".._bad___name__x.mcap");
    EXPECT_EQ(srv::sanitize_display_filename(".."), "artifact.bin");
}

TEST_F(ServerContractTest, WireDtoSerializationAndOpenApiProjectionMatchV1Contract) {
    // Verify 64-bit integers beyond JS Number.MAX_SAFE_INTEGER round-trip as decimal strings.
    rodaq::TaskSnapshot snap;
    snap.control_generation = 9007199254740993ull;  // 2^53 + 1
    snap.snapshot_sequence = 18446744073709551615ull;
    snap.desired_state = rodaq::DesiredState::Running;
    snap.execution_condition = rodaq::ExecutionCondition::Active;
    snap.data_impact = rodaq::DataImpact::None;
    snap.active_config_version = "sha256:abc";
    const auto dto = srv::to_task_dto(snap, std::string("arm-cell"));
    EXPECT_EQ(dto["control_generation"], "9007199254740993");
    EXPECT_EQ(dto["snapshot_sequence"], "18446744073709551615");
    EXPECT_EQ(dto["desired_state"], "running");
    EXPECT_EQ(dto["execution_condition"], "active");
    EXPECT_EQ(dto["data_impact"], "none");

    // Verify strict lifecycle operation parser rejects unknown fields and non-empty parameters.
    EXPECT_EQ(
        srv::parse_lifecycle_operation_request(R"({"kind":"start","parameters":{}})").value(),
        rodaq::OperationKind::Start);
    EXPECT_FALSE(
        srv::parse_lifecycle_operation_request(R"({"kind":"start","parameters":{"extra":1}})")
            .ok());
    EXPECT_FALSE(
        srv::parse_lifecycle_operation_request(R"({"kind":"start","kind":"stop","parameters":{}})")
            .ok());

    // Verify OpenAPI 3.1 projection exists and declares every required V1 route under /api/ (no
    // /api/v1/).
    std::ifstream spec_in(RODAQ_OPENAPI_PATH);
    ASSERT_TRUE(spec_in.good());
    std::ostringstream ss;
    ss << spec_in.rdbuf();
    const auto spec = ss.str();
    EXPECT_NE(spec.find("openapi: 3.1.0"), std::string::npos);
    EXPECT_EQ(spec.find("/api/v1/"), std::string::npos);
    for (const auto* route :
         {"/api/session:", "/api/system:", "/api/capabilities:", "/api/tasks:",
          "/api/tasks/{task_id}:", "/api/tasks/{task_id}/config:",
          "/api/tasks/{task_id}/operations:", "/api/tasks/{task_id}/operations/{operation_id}:",
          "/api/tasks/{task_id}/runtime:", "/api/artifacts:", "/api/artifacts/{artifact_id}:",
          "/api/artifacts/{artifact_id}/files/{file_id}:"}) {
        EXPECT_NE(spec.find(route), std::string::npos)
            << "Missing route in openapi.yaml: " << route;
    }
}

}  // namespace
