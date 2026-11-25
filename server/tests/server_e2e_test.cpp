#include "daq/startup.hpp"
#include "security/auth.hpp"
#include "server_app.hpp"
#include "server_config.hpp"
#include "service_config.hpp"

#include <drogon/HttpClient.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace {

namespace fs = std::filesystem;
namespace daq = rodaq::daq;
namespace srv = rodaq::server;
using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::uint16_t kTestPort = 18492;

class ServerE2eTest : public testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/rodaq-s5-e2e-XXXXXX";
        const auto* dir = ::mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        root_ = dir;
        state_root_ = root_ / "state";
        volume_root_ = root_ / "volume";
        packages_root_ = root_ / "packages";
        web_root_ = root_ / "web";
        cred_path_ = root_ / "server_credentials.json";

        fs::create_directory(state_root_);
        fs::create_directory(volume_root_);
        fs::create_directory(packages_root_);
        fs::create_directories(web_root_ / "assets");

        // Stage fake plugins with 0500 permissions.
        fs::copy(RODAQ_S5_STORAGE_PACKAGE, packages_root_ / "storage", fs::copy_options::recursive);
        fs::copy(
            RODAQ_S5_ACQUISITION_PACKAGE, packages_root_ / "acquisition",
            fs::copy_options::recursive);
        for (const auto& entry : fs::recursive_directory_iterator(packages_root_)) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(packages_root_, fs::perms::owner_read | fs::perms::owner_exec);

        // Stage minimal web dist bundle with web_manifest.json.
        {
            std::ofstream out(web_root_ / "web_manifest.json");
            out << R"({"web_build_id":"test-build-1","api_major":1,"api_minor_min":0})";
        }
        {
            std::ofstream out(web_root_ / "index.html");
            out << "<!doctype html><html><body>RoDAQ Web Test</body></html>";
        }
        {
            std::ofstream out(web_root_ / "assets/app.deadbeef.js");
            out << "console.log('rodaq');";
        }

        // Stage credentials (operator=read+control, admin=read+control+admin).
        op_bearer_ = "0123456789abcdef0123456789abcdef_e2e_operator_token";
        admin_bearer_ = "fedcba9876543210fedcba9876543210_e2e_admin_token_99";
        const auto op_v = srv::compute_bearer_verifier(op_bearer_).value();
        const auto adm_v = srv::compute_bearer_verifier(admin_bearer_).value();
        {
            std::ofstream out(cred_path_, std::ios::binary | std::ios::trunc);
            out << Json{
                       {"format_version", "1.0"},
                       {"credentials",
                        {{{"credential_id", "op"},
                          {"principal", "operator"},
                          {"scopes", {"read", "control"}},
                          {"verifier", op_v},
                          {"credential_epoch", "1"},
                          {"enabled", true}},
                         {{"credential_id", "adm"},
                          {"principal", "admin"},
                          {"scopes", {"read", "control", "admin"}},
                          {"verifier", adm_v},
                          {"credential_epoch", "1"},
                          {"enabled", true}}}}}
                       .dump();
        }
        ASSERT_EQ(::chmod(cred_path_.c_str(), 0600), 0);

        // Open real Core Composition.
        daq::StartupOptions opts;
        opts.state_root = state_root_;
        opts.volume_root = volume_root_;
        opts.mode = daq::DeploymentOpenMode::Initialize;
        opts.plugins.search_paths = {packages_root_};
        opts.plugins.max_loaded_plugins = 4;
        opts.plugins.max_instances = 16;
        opts.plugins.max_config_size = 4096;
        opts.plugins.shutdown_timeout = 5s;
        opts.capacity.max_managed_bytes = 64ull * 1024 * 1024;
        opts.capacity.max_commit_bytes = 16ull * 1024 * 1024;
        opts.capacity.stop_free_bytes = 1024;
        opts.capacity.low_free_bytes = 2048;
        opts.capacity.recovery_free_bytes = 4096;
        opts.capacity.recovery_stable_ns = 1;
        opts.capacity.space_sample_max_age_ns = 60'000'000'000ull;
        opts.ledger_records = 64;
        opts.settings.deployment_generation = 1;
        opts.settings.qualified_runtime_profile_id = "s5-e2e-profile";
        opts.settings.output_root = "local";
        opts.settings.max_tasks = 2;
        opts.settings.artifact_audit_window = 10ms;
        daq::ResourceProfile profile;
        profile.name = "basic";
        profile.instance.records = 64;
        profile.instance.logical_bytes = 4096;
        profile.instance.payloads = {65536, 4096, 256, 64};
        profile.instance.queue_capacity = 32;
        profile.instance.callback_concurrency = 2;
        profile.storage.header.struct_size = sizeof(rodaq::storage::PrepareConfig);
        profile.storage.max_record_bytes = 256;
        profile.storage.max_batch_bytes = 4096;
        profile.storage.max_batch_records = 16;
        profile.storage.max_descriptors_per_segment = 32;
        profile.storage.max_segment_records = 10000;
        profile.storage.max_segment_input_bytes = 2 * 1024 * 1024;
        profile.storage.max_segment_physical_bytes = 4 * 1024 * 1024;
        profile.storage.max_segment_file_count = 8;
        profile.storage.max_relative_path_bytes = 128;
        opts.settings.profiles.push_back(profile);
        opts.settings.allowed_plugins = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        opts.settings.storage = {{"rodaq.fake.storage", "1.0.0", "fake"}};

        auto comp = daq::Composition::open(std::move(opts));
        ASSERT_TRUE(comp.ok()) << static_cast<int>(comp.status().code());
        composition_ = std::move(comp.value());
        ASSERT_TRUE(composition_->observe().ok());

        srv::ServerConfig scfg;
        scfg.http.listen_address = "127.0.0.1";
        scfg.http.port = kTestPort;
        scfg.http.worker_threads = 2;
        scfg.security.credential_store = cred_path_;
        scfg.static_asset_root = web_root_;
        srv::ServerServices services;
        services.tasks = &composition_->tasks();
        services.system = &composition_->system();
        services.artifacts = &composition_->artifacts();
        services.core_shutdown = [this](auto dl) { return composition_->shutdown(dl); };

        // Start in Recovering mode first to verify limited-mode enforcement!
        ASSERT_TRUE(server_.start(std::move(scfg), std::move(services), false).ok());
        client_ =
            drogon::HttpClient::newHttpClient("http://127.0.0.1:" + std::to_string(kTestPort));
    }

    void TearDown() override {
        client_.reset();
        (void)server_.shutdown(std::chrono::steady_clock::now() + 5s);
        composition_.reset();
        if (!root_.empty()) {
            std::error_code ec;
            fs::permissions(packages_root_, fs::perms::owner_all, ec);
            for (const auto& entry : fs::recursive_directory_iterator(packages_root_, ec)) {
                fs::permissions(entry.path(), fs::perms::owner_all, ec);
            }
            fs::remove_all(root_, ec);
        }
    }

    drogon::HttpRequestPtr make_bearer_req(
        drogon::HttpMethod method, const std::string& path, const std::string& bearer,
        const std::string& body = "") {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(method);
        req->setPath(path);
        req->addHeader("Host", "127.0.0.1:" + std::to_string(kTestPort));
        req->addHeader("RoDAQ-API-Version", "1.0");
        req->addHeader("Authorization", "Bearer " + bearer);
        if (!body.empty()) {
            req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            req->setBody(body);
        }
        return req;
    }

    std::string portable_task_json() const {
        auto cfg = rodaq::test::service_config().value();
        cfg.pipelines[0].acquisition = {
            "rodaq.fake.acquisition", "1.0.0", R"({"streams":["sample"]})"};
        cfg.pipelines[0].streams[0].stream_key = "sample";
        cfg.pipelines[0].storage = {"rodaq.fake.storage", "1.0.0", "{}"};
        return rodaq::export_task_config(cfg).value();
    }

    fs::path root_;
    fs::path state_root_;
    fs::path volume_root_;
    fs::path packages_root_;
    fs::path web_root_;
    fs::path cred_path_;
    std::string op_bearer_;
    std::string admin_bearer_;
    std::unique_ptr<daq::Composition> composition_;
    srv::ServerApp server_;
    drogon::HttpClientPtr client_;
};

TEST_F(ServerE2eTest, FullHttpLifecycleAuthIdempotencyArtifactStreamingAndStaticSpa) {
    // 1. Recovering limited-mode: /api/system returns 200 recovering; /api/tasks returns 503
    // recovering.
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Get, "/api/system", op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        const auto doc = Json::parse(resp->body());
        EXPECT_EQ(doc["data"]["server_phase"], "recovering");
        EXPECT_EQ(resp->getHeader("RoDAQ-API-Version"), "1.0");
        EXPECT_FALSE(resp->getHeader("RoDAQ-Request-ID").empty());
        EXPECT_EQ(resp->getHeader("X-Content-Type-Options"), "nosniff");
    }
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Get, "/api/tasks", op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k503ServiceUnavailable);
    }

    // Promote to Ready.
    server_.mark_ready();

    // 2. Missing API version returns 400 missing_api_version; /api/unknown returns JSON 404 (not
    // SPA).
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/api/system");
        req->addHeader("Host", "127.0.0.1:" + std::to_string(kTestPort));
        req->addHeader("Authorization", "Bearer " + op_bearer_);
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
        EXPECT_EQ(Json::parse(resp->body())["error"]["code"], "missing_api_version");
    }
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Get, "/api/does-not-exist", op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k404NotFound);
        EXPECT_EQ(Json::parse(resp->body())["error"]["code"], "not_found");
    }

    // 3. Static SPA serving + hashed asset long cache + index.html no-cache.
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/tasks/some-spa-route");
        req->addHeader("Host", "127.0.0.1:" + std::to_string(kTestPort));
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k200OK);
        EXPECT_EQ(resp->getHeader("Cache-Control"), "no-cache");
        EXPECT_NE(resp->body().find("RoDAQ Web Test"), std::string::npos);
    }
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/assets/app.deadbeef.js");
        req->addHeader("Host", "127.0.0.1:" + std::to_string(kTestPort));
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k200OK);
        EXPECT_EQ(resp->getHeader("Cache-Control"), "public, max-age=31536000, immutable");
    }

    // 4. Browser session login (POST /api/session), GET /api/session recovery, and CSRF rejection.
    std::string session_cookie;
    std::string csrf_token;
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Post, "/api/session", op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        const auto doc = Json::parse(resp->body());
        EXPECT_EQ(doc["data"]["principal"], "operator");
        csrf_token = doc["data"]["csrf_token"].get<std::string>();
        ASSERT_FALSE(csrf_token.empty());
        session_cookie = resp->getCookie("rodaq_session").value();
        ASSERT_FALSE(session_cookie.empty());
    }
    {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/api/session");
        req->addHeader("Host", "127.0.0.1:" + std::to_string(kTestPort));
        req->addCookie("rodaq_session", session_cookie);
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        EXPECT_EQ(Json::parse(resp->body())["data"]["csrf_token"], csrf_token);
    }

    // 5. Create Task (201) + same Idempotency-Key retry returns identical task_id + Raw Config
    // export.
    const auto cfg_body = portable_task_json();
    std::string task_id;
    {
        auto req = make_bearer_req(drogon::Post, "/api/tasks", op_bearer_, cfg_body);
        req->addHeader("Idempotency-Key", "create-e2e-1");
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k201Created) << resp->body();
        const auto doc = Json::parse(resp->body());
        task_id = doc["data"]["task_id"].get<std::string>();
        ASSERT_FALSE(task_id.empty());
    }
    {
        auto req = make_bearer_req(drogon::Post, "/api/tasks", op_bearer_, cfg_body);
        req->addHeader("Idempotency-Key", "create-e2e-1");
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k201Created);
        EXPECT_EQ(Json::parse(resp->body())["data"]["task_id"], task_id);
    }
    {
        auto [res, resp] = client_->sendRequest(
            make_bearer_req(drogon::Get, "/api/tasks/" + task_id + "/config", op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        const auto raw = Json::parse(resp->body());
        // Raw Portable TaskConfig has NO API envelope!
        EXPECT_FALSE(raw.contains("api_version"));
        EXPECT_TRUE(raw.contains("pipelines"));
        EXPECT_FALSE(resp->getHeader("ETag").empty());
    }

    // 6. Start -> record -> Stop via POST /api/tasks/{id}/operations (202 Accepted) + poll
    // operation.
    auto submit_op =
        [&](const std::string& kind, const std::string& key,
            const std::string& bearer) -> std::pair<drogon::HttpStatusCode, std::string> {
        auto req = make_bearer_req(
            drogon::Post, "/api/tasks/" + task_id + "/operations", bearer,
            std::string(R"({"kind":")") + kind + R"(","parameters":{}})");
        req->addHeader("Idempotency-Key", key);
        auto [res, resp] = client_->sendRequest(req);
        EXPECT_EQ(res, drogon::ReqResult::Ok);
        if (resp->statusCode() != drogon::k202Accepted) {
            return {resp->statusCode(), std::string(resp->body())};
        }
        return {
            resp->statusCode(),
            Json::parse(resp->body())["data"]["operation_id"].get<std::string>()};
    };

    auto wait_op = [&](const std::string& op_id) -> std::string {
        const auto dl = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < dl) {
            auto [res, resp] = client_->sendRequest(make_bearer_req(
                drogon::Get, "/api/tasks/" + task_id + "/operations/" + op_id, op_bearer_));
            if (res == drogon::ReqResult::Ok && resp->statusCode() == drogon::k200OK) {
                const auto st = Json::parse(resp->body())["data"]["state"].get<std::string>();
                if (st == "succeeded" || st == "failed") {
                    return st;
                }
            }
            std::this_thread::sleep_for(15ms);
        }
        return "timeout";
    };

    const auto [s_code, start_op_id] = submit_op("start", "op-start-1", op_bearer_);
    ASSERT_EQ(s_code, drogon::k202Accepted);
    ASSERT_EQ(wait_op(start_op_id), "succeeded");
    std::this_thread::sleep_for(80ms);

    // Check /api/tasks/{id}/runtime while running.
    {
        auto [res, resp] = client_->sendRequest(
            make_bearer_req(drogon::Get, "/api/tasks/" + task_id + "/runtime", op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        const auto doc = Json::parse(resp->body());
        ASSERT_EQ(doc["data"]["pipelines"].size(), 1u);
        EXPECT_EQ(doc["data"]["pipelines"][0]["streams"].size(), 1u);
    }

    const auto [st_code, stop_op_id] = submit_op("stop", "op-stop-1", op_bearer_);
    ASSERT_EQ(st_code, drogon::k202Accepted);
    ASSERT_EQ(wait_op(stop_op_id), "succeeded");

    // 7. Query artifacts, HEAD file, GET full (200), GET Range (206), Multiple Range (400),
    // Unsatisfiable (416).
    std::string artifact_id;
    std::string file_id;
    std::string etag;
    std::uint64_t file_size = 0;
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Get, "/api/artifacts", op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        const auto doc = Json::parse(resp->body());
        ASSERT_EQ(doc["data"]["items"].size(), 1u);
        artifact_id = doc["data"]["items"][0]["artifact_id"].get<std::string>();
    }
    {
        auto [res, resp] = client_->sendRequest(
            make_bearer_req(drogon::Get, "/api/artifacts/" + artifact_id, op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        const auto doc = Json::parse(resp->body());
        ASSERT_FALSE(doc["data"]["files"].empty());
        file_id = doc["data"]["files"][0]["file_id"].get<std::string>();
        file_size = std::stoull(doc["data"]["files"][0]["size"].get<std::string>());
        EXPECT_GT(file_size, 0u);
    }
    const std::string file_path = "/api/artifacts/" + artifact_id + "/files/" + file_id;
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Head, file_path, op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        EXPECT_EQ(resp->getHeader("Accept-Ranges"), "bytes");
        EXPECT_EQ(resp->getHeader("Cache-Control"), "private, no-store");
        etag = resp->getHeader("ETag");
        ASSERT_FALSE(etag.empty());
    }
    std::string full_body;
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Get, file_path, op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k200OK);
        full_body = std::string(resp->body());
        EXPECT_EQ(full_body.size(), file_size);
    }
    {
        auto req = make_bearer_req(drogon::Get, file_path, op_bearer_);
        req->addHeader("Range", "bytes=0-15");
        req->addHeader("If-Range", etag);
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        ASSERT_EQ(resp->statusCode(), drogon::k206PartialContent);
        EXPECT_EQ(resp->body().size(), 16u);
        EXPECT_EQ(resp->body(), full_body.substr(0, 16));
    }
    {
        auto req = make_bearer_req(drogon::Get, file_path, op_bearer_);
        req->addHeader("Range", "bytes=0-3, 5-7");
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
        EXPECT_EQ(Json::parse(resp->body())["error"]["code"], "multiple_ranges_not_supported");
    }
    {
        auto req = make_bearer_req(drogon::Get, file_path, op_bearer_);
        req->addHeader("Range", "bytes=999999999-");
        auto [res, resp] = client_->sendRequest(req);
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k416RequestedRangeNotSatisfiable);
        EXPECT_EQ(resp->getHeader("Content-Range"), "bytes */" + std::to_string(file_size));
    }

    // 8. Operator (control scope) MUST receive 403 on Reset; Admin succeeds on Reset & Retire!
    {
        const auto [r_code, _] = submit_op("reset", "op-reset-forbidden", op_bearer_);
        EXPECT_EQ(r_code, drogon::k403Forbidden);
    }
    {
        const auto [r_code, reset_op_id] = submit_op("reset", "op-reset-admin", admin_bearer_);
        ASSERT_EQ(r_code, drogon::k202Accepted);
        ASSERT_EQ(wait_op(reset_op_id), "succeeded");
    }
    {
        const auto [ret_code, retire_op_id] = submit_op("retire", "op-retire-admin", admin_bearer_);
        ASSERT_EQ(ret_code, drogon::k202Accepted);
        EXPECT_EQ(wait_op(retire_op_id), "succeeded");
    }
    {
        auto [res, resp] =
            client_->sendRequest(make_bearer_req(drogon::Get, "/api/tasks/" + task_id, op_bearer_));
        ASSERT_EQ(res, drogon::ReqResult::Ok);
        EXPECT_EQ(resp->statusCode(), drogon::k404NotFound);
    }
}

}  // namespace
