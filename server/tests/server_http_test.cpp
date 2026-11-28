#include "api/json_codec.hpp"
#include "config.hpp"
#include "security/auth.hpp"
#include "server_app.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <rodaq/daq/runtime.hpp>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using namespace rodaq::server;

std::uint16_t pick_free_loopback_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 19080;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    std::uint16_t port = 19080;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    ::close(fd);
    return port;
}

struct HttpClientResponse {
    int status{0};
    std::map<std::string, std::string> headers;
    std::string body;

    std::string header(const std::string& key) const {
        const auto it = headers.find(key);
        return it == headers.end() ? std::string{} : it->second;
    }
};

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

HttpClientResponse send_http_request(
    std::uint16_t port, const std::string& method, const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    const std::string& body = "", bool has_custom_host = false) {
    HttpClientResponse out;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return out;
    }
    timeval tv{};
    tv.tv_sec = 10;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return out;
    }

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n";
    if (!has_custom_host) {
        req << "Host: 127.0.0.1:" << port << "\r\n";
    }
    req << "Connection: close\r\n";
    for (const auto& kv : headers) {
        req << kv.first << ": " << kv.second << "\r\n";
    }
    if (!body.empty()) {
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n" << body;
    const std::string wire = req.str();
    std::size_t written = 0;
    while (written < wire.size()) {
        const ssize_t n = ::send(fd, wire.data() + written, wire.size() - written, 0);
        if (n <= 0) {
            break;
        }
        written += static_cast<std::size_t>(n);
    }

    std::string raw;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        raw.append(buf, static_cast<std::size_t>(n));
        const auto hend = raw.find("\r\n\r\n");
        if (hend != std::string::npos && raw.rfind("HTTP/1.1 204 ", 0) == 0) {
            break;
        }
    }
    ::close(fd);

    const auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        return out;
    }
    std::istringstream hs(raw.substr(0, hdr_end));
    std::string status_line;
    std::getline(hs, status_line);
    const auto sp1 = status_line.find(' ');
    if (sp1 != std::string::npos) {
        out.status = std::atoi(status_line.c_str() + sp1 + 1);
    }
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = to_lower_ascii(line.substr(0, colon));
        std::string val = line.substr(colon + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        out.headers[key] = val;
    }
    out.body = raw.substr(hdr_end + 4);
    return out;
}

void set_readonly_tree(const fs::path& root) {
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_directory()) {
            dirs.push_back(entry.path());
        } else {
            fs::permissions(entry.path(), fs::perms::owner_read);
        }
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        fs::permissions(*it, fs::perms::owner_read | fs::perms::owner_exec);
    }
    fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec);
}

void restore_writable_tree(const fs::path& root) noexcept {
    std::error_code ignored;
    if (!fs::exists(root, ignored)) {
        return;
    }
    fs::permissions(root, fs::perms::owner_all, ignored);
    for (const auto& entry : fs::recursive_directory_iterator(root, ignored)) {
        fs::permissions(entry.path(), fs::perms::owner_all, ignored);
    }
}

std::string make_portable_task_json(const std::string& name) {
    return nlohmann::json{
        {"format_version", "1.0"},
        {"name", name},
        {"segment_policy", {{"max_duration_ms", 60000}, {"max_physical_bytes", nullptr}}},
        {"retention_policy",
         {{"mode", "keep_all"}, {"max_duration_ms", nullptr}, {"max_physical_bytes", nullptr}}},
        {"pipelines",
         {{{"required", true},
           {"resource_profile", "default"},
           {"output_root", "local"},
           {"acquisition",
            {{"plugin_id", "rodaq.fake.acquisition"},
             {"plugin_version", "1.0.0"},
             {"config", {{"streams", {"s0"}}, {"period_us", 2000}}}}},
           {"storage",
            {{"plugin_id", "rodaq.fake.storage"},
             {"plugin_version", "1.0.0"},
             {"config", nlohmann::json::object()}}},
           {"streams",
            {{{"stream_key", "s0"}, {"name", "s0"}, {"readiness_policy", "entry_ready"}}}}}}}}
        .dump();
}

TEST(ServerHttpIntegrationTest, FullLifecycleSecurityDownloadRangeAndStaticSpa) {
    const fs::path root =
        fs::temp_directory_path() /
        ("rodaq-server-http-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "packages");
    fs::create_directories(root / "state");
    fs::create_directories(root / "volume");
    fs::create_directories(root / "web/assets");
    struct Cleanup {
        fs::path r;
        ~Cleanup() {
            restore_writable_tree(r / "packages");
            std::error_code ec;
            fs::remove_all(r, ec);
        }
    } cleanup{root};

    fs::copy(
        fs::path(RODAQ_SERVER_STORAGE_PACKAGE) / "rodaq.fake.storage",
        root / "packages" / "rodaq.fake.storage", fs::copy_options::recursive);
    fs::copy(
        fs::path(RODAQ_SERVER_ACQUISITION_PACKAGE) / "rodaq.fake.acquisition",
        root / "packages" / "rodaq.fake.acquisition", fs::copy_options::recursive);
    set_readonly_tree(root / "packages");

    {
        std::ofstream(root / "web/index.html") << "<!doctype html><html><body>RoDAQ</body></html>";
        std::ofstream(root / "web/assets/app.hash123.js") << "export const ok = true;";
        std::ofstream(root / "web/web_manifest.json")
            << R"({"web_build_id":"test-1","api_major":1,"api_minor_min":0})";
    }

    const std::string bearer_tok = "rodaq_local_bearer_secret_with_256bit_entropy_0001";
    const fs::path token_file = root / "server.token";
    {
        std::ofstream(token_file) << bearer_tok << "\n";
        ASSERT_EQ(::chmod(token_file.c_str(), 0600), 0);
    }

    // 1. Open real DaqRuntime synchronously BEFORE ServerApp starts Ready.
    rodaq::DaqOpenOptions daq_opts;
    daq_opts.state_root = root / "state";
    daq_opts.volume_root = root / "volume";
    daq_opts.mode = rodaq::DaqOpenMode::Initialize;
    daq_opts.plugin_search_paths = {root / "packages"};
    daq_opts.qualified_runtime_profile_id = "s5-http-test";
    daq_opts.max_tasks = 2;
    daq_opts.max_pipelines_per_task = 2;
    daq_opts.volume.name = "local";
    daq_opts.volume.max_managed_bytes = 256ull * 1024 * 1024;
    daq_opts.volume.max_commit_bytes = 32ull * 1024 * 1024;
    daq_opts.volume.stop_free_bytes = 1024;
    daq_opts.volume.low_free_bytes = 2048;
    daq_opts.volume.recovery_free_bytes = 4096;
    daq_opts.volume.max_segments = 8;
    daq_opts.volume.max_commit_slots = 4;
    daq_opts.volume.max_artifacts_per_volume = 64;
    rodaq::DaqResourceProfile profile;
    profile.name = "default";
    profile.max_records = 64;
    profile.max_logical_bytes = 4096;
    profile.payload_pool_bytes = 32768;
    profile.max_payload_bytes = 256;
    profile.payload_min_chunk_bytes = 256;
    profile.payload_max_chunks = 64;
    profile.queue_capacity = 32;
    profile.callback_concurrency = 2;
    profile.max_segment_physical_bytes = 1024 * 1024;
    profile.segment_reservation_bytes = 1280 * 1024;
    profile.max_segment_records = 1000;
    profile.max_segment_input_bytes = 256 * 1024;
    profile.max_batch_records = 16;
    profile.max_batch_input_bytes = 4096;
    profile.max_streams = 8;
    profile.max_descriptors = 16;
    profile.max_sealed_files = 4;
    profile.max_manifest_bytes = 65536;
    daq_opts.profiles.push_back(std::move(profile));
    daq_opts.allowed_backends.push_back(
        {"rodaq.fake.acquisition", "1.0.0", rodaq::BackendRole::Acquisition, "",
         rodaq::SupportStatus::QualifiedCandidate, rodaq::QualificationStatus::Qualified});
    daq_opts.allowed_backends.push_back(
        {"rodaq.fake.storage", "1.0.0", rodaq::BackendRole::Storage, "fake",
         rodaq::SupportStatus::Required, rodaq::QualificationStatus::Qualified});

    auto runtime = rodaq::DaqRuntime::open(std::move(daq_opts));
    ASSERT_TRUE(runtime.ok()) << runtime.status().message();
    (void)runtime.value()->observe();

    const std::uint16_t port = pick_free_loopback_port();
    ServerConfig srv_cfg;
    srv_cfg.http.listen_address = "127.0.0.1";
    srv_cfg.http.port = port;
    srv_cfg.http.worker_threads = 2;
    srv_cfg.security.bearer_token_file = token_file;
    srv_cfg.download.chunk_bytes = 65536;
    srv_cfg.download.max_bytes_in_flight = 131072;
    srv_cfg.static_assets_root = root / "web";
    ASSERT_TRUE(validate_server_config(srv_cfg).ok());

    ServerApp server;
    ServerServices services;
    services.tasks = &runtime.value()->tasks();
    services.system = &runtime.value()->system();
    services.artifacts = &runtime.value()->artifacts();
    ASSERT_TRUE(server.start(srv_cfg, std::move(services)).ok());

    struct RuntimeServerShutdownGuard {
        ServerApp* srv;
        rodaq::DaqRuntime* rt;
        bool done{false};
        ~RuntimeServerShutdownGuard() {
            if (!done) {
                const auto dl = std::chrono::steady_clock::now() + 5s;
                if (srv) {
                    (void)srv->shutdown(dl);
                }
                if (rt) {
                    (void)rt->shutdown(dl);
                }
            }
        }
    } shutdown_guard{&server, runtime.value().get(), false};

    const std::vector<std::pair<std::string, std::string>> hdrs = {
        {"Authorization", "Bearer " + bearer_tok}, {"RoDAQ-API-Version", "1.0"}};

    // 2. Host, Origin, API version, Bearer auth, and absence of /api/session.
    EXPECT_EQ(
        send_http_request(port, "GET", "/api/system", {{"Host", "evil.example:8080"}}, "", true)
            .status,
        403);
    EXPECT_EQ(
        send_http_request(port, "GET", "/api/system", {{"Authorization", "Bearer " + bearer_tok}})
            .status,
        400);
    EXPECT_EQ(
        send_http_request(
            port, "GET", "/api/system",
            {{"Authorization", "Bearer " + bearer_tok}, {"RoDAQ-API-Version", "2.0"}})
            .status,
        400);
    EXPECT_EQ(
        send_http_request(port, "GET", "/api/system", {{"RoDAQ-API-Version", "1.0"}}).status, 401);
    EXPECT_EQ(send_http_request(port, "POST", "/api/session", hdrs).status, 404);

    // Mismatched Origin rejected 403 even with valid Bearer.
    auto bad_origin = hdrs;
    bad_origin.push_back({"Origin", "http://evil.example"});
    bad_origin.push_back({"Content-Type", "application/json"});
    bad_origin.push_back({"Idempotency-Key", "orig-bad-1"});
    EXPECT_EQ(
        send_http_request(port, "POST", "/api/tasks", bad_origin, make_portable_task_json("x"))
            .status,
        403);

    // 3. System & Capabilities snapshots (no recovery, no idempotency_retention, numeric counts).
    const auto sys_resp = send_http_request(port, "GET", "/api/system", hdrs);
    ASSERT_EQ(sys_resp.status, 200);
    EXPECT_EQ(sys_resp.header("rodaq-api-version"), "1.0");
    EXPECT_FALSE(sys_resp.header("rodaq-request-id").empty());
    const auto sys_data = nlohmann::json::parse(sys_resp.body)["data"];
    EXPECT_EQ(sys_data["server_phase"], "ready");
    EXPECT_FALSE(sys_data.contains("recovery"));
    EXPECT_TRUE(sys_data["resources"]["rss_bytes"].is_number_unsigned());
    EXPECT_TRUE(sys_data["freshness"].contains("age_ms"));

    const auto caps = send_http_request(port, "GET", "/api/capabilities", hdrs);
    ASSERT_EQ(caps.status, 200);
    const auto caps_json = nlohmann::json::parse(caps.body)["data"];
    EXPECT_FALSE(caps_json["backends"].empty());
    EXPECT_EQ(caps_json["output_roots"][0]["name"], "local");
    EXPECT_EQ(caps_json["resource_profiles"][0]["name"], "default");
    EXPECT_FALSE(caps_json["api_limits"].contains("idempotency_retention"));
    EXPECT_TRUE(caps_json["api_limits"]["recommended_polling_floor_ms"].is_number_unsigned());

    // 4. Task create, idempotency, raw config export, start/runtime/stop.
    const std::string task_json = make_portable_task_json("http-task-1");
    auto create_hdrs = hdrs;
    create_hdrs.push_back({"Content-Type", "application/json"});
    create_hdrs.push_back({"Idempotency-Key", "http-create-1"});
    const auto created = send_http_request(port, "POST", "/api/tasks", create_hdrs, task_json);
    ASSERT_EQ(created.status, 201) << created.body;
    const std::string task_id = nlohmann::json::parse(created.body)["data"]["task_id"];

    const auto created_dup = send_http_request(port, "POST", "/api/tasks", create_hdrs, task_json);
    ASSERT_EQ(created_dup.status, 201);
    EXPECT_EQ(nlohmann::json::parse(created_dup.body)["data"]["task_id"], task_id);

    const auto created_conflict = send_http_request(
        port, "POST", "/api/tasks", create_hdrs, make_portable_task_json("different-name"));
    EXPECT_EQ(created_conflict.status, 409);

    const auto exported = send_http_request(port, "GET", "/api/tasks/" + task_id + "/config", hdrs);
    ASSERT_EQ(exported.status, 200);
    EXPECT_FALSE(exported.header("etag").empty());
    const auto exported_json = nlohmann::json::parse(exported.body);
    EXPECT_FALSE(exported_json.contains("api_version"));
    EXPECT_EQ(exported_json["name"], "http-task-1");

    // Stale If-Control-Generation returns 409.
    auto stale_hdrs = hdrs;
    stale_hdrs.push_back({"Content-Type", "application/json"});
    stale_hdrs.push_back({"Idempotency-Key", "stale-start-1"});
    stale_hdrs.push_back({"If-Control-Generation", "99"});
    EXPECT_EQ(
        send_http_request(
            port, "POST", "/api/tasks/" + task_id + "/operations", stale_hdrs,
            R"({"kind":"start","parameters":{}})")
            .status,
        409);

    auto start_hdrs = hdrs;
    start_hdrs.push_back({"Content-Type", "application/json"});
    start_hdrs.push_back({"Idempotency-Key", "start-1"});
    start_hdrs.push_back({"If-Control-Generation", "1"});
    const auto start_resp = send_http_request(
        port, "POST", "/api/tasks/" + task_id + "/operations", start_hdrs,
        R"({"kind":"start","parameters":{}})");
    ASSERT_EQ(start_resp.status, 202) << start_resp.body;
    const std::string start_op_id = nlohmann::json::parse(start_resp.body)["data"]["operation_id"];

    auto wait_op_http = [&](const std::string& op_id) -> std::string {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            (void)runtime.value()->observe();
            const auto r = send_http_request(
                port, "GET", "/api/tasks/" + task_id + "/operations/" + op_id, hdrs);
            if (r.status == 200) {
                const std::string st = nlohmann::json::parse(r.body)["data"]["state"];
                if (st == "succeeded" || st == "failed") {
                    return st;
                }
            }
            std::this_thread::sleep_for(15ms);
        }
        return "timeout";
    };
    ASSERT_EQ(wait_op_http(start_op_id), "succeeded");

    std::this_thread::sleep_for(120ms);
    (void)runtime.value()->observe();

    const auto rt_resp = send_http_request(port, "GET", "/api/tasks/" + task_id + "/runtime", hdrs);
    ASSERT_EQ(rt_resp.status, 200);
    const auto rt_json = nlohmann::json::parse(rt_resp.body)["data"];
    ASSERT_EQ(rt_json["pipelines"].size(), 1u);
    ASSERT_EQ(rt_json["pipelines"][0]["streams"].size(), 1u);

    auto stop_hdrs = hdrs;
    stop_hdrs.push_back({"Content-Type", "application/json"});
    stop_hdrs.push_back({"Idempotency-Key", "stop-1"});
    const auto stop_resp = send_http_request(
        port, "POST", "/api/tasks/" + task_id + "/operations", stop_hdrs,
        R"({"kind":"stop","parameters":{}})");
    ASSERT_EQ(stop_resp.status, 202) << stop_resp.body;
    ASSERT_EQ(
        wait_op_http(nlohmann::json::parse(stop_resp.body)["data"]["operation_id"]), "succeeded");

    // 5. Artifact query, detail, HEAD, full GET, Range 206, 400 multiple, 416, If-Range,
    // disconnect.
    const auto arts = send_http_request(port, "GET", "/api/artifacts?task_id=" + task_id, hdrs);
    ASSERT_EQ(arts.status, 200);
    const auto art_items = nlohmann::json::parse(arts.body)["data"]["items"];
    ASSERT_FALSE(art_items.empty());
    const std::string art_id = art_items[0]["artifact_id"];

    const auto art_detail = send_http_request(port, "GET", "/api/artifacts/" + art_id, hdrs);
    ASSERT_EQ(art_detail.status, 200);
    const auto detail_data = nlohmann::json::parse(art_detail.body)["data"];
    ASSERT_FALSE(detail_data["files"].empty());
    const std::string download_url = detail_data["files"][0]["download_url"];
    const std::string etag = detail_data["files"][0]["etag"];
    const std::uint64_t expected_size = detail_data["files"][0]["size"].get<std::uint64_t>();
    ASSERT_GT(expected_size, 16u);

    const auto head_resp = send_http_request(port, "HEAD", download_url, hdrs);
    ASSERT_EQ(head_resp.status, 200);
    EXPECT_EQ(head_resp.header("accept-ranges"), "bytes");
    EXPECT_EQ(head_resp.header("content-length"), std::to_string(expected_size));
    EXPECT_EQ(head_resp.header("etag"), "\"" + etag + "\"");
    EXPECT_TRUE(head_resp.body.empty());

    const auto full_get = send_http_request(port, "GET", download_url, hdrs);
    ASSERT_EQ(full_get.status, 200);
    EXPECT_EQ(full_get.body.size(), expected_size);

    auto range_hdrs = hdrs;
    range_hdrs.push_back({"Range", "bytes=0-15"});
    range_hdrs.push_back({"If-Range", "\"" + etag + "\""});
    const auto part_get = send_http_request(port, "GET", download_url, range_hdrs);
    ASSERT_EQ(part_get.status, 206);
    EXPECT_EQ(part_get.header("content-range"), "bytes 0-15/" + std::to_string(expected_size));
    EXPECT_EQ(part_get.body, full_get.body.substr(0, 16));

    auto mismatch_hdrs = hdrs;
    mismatch_hdrs.push_back({"Range", "bytes=0-15"});
    mismatch_hdrs.push_back({"If-Range", "\"wrong-etag\""});
    const auto mismatch_get = send_http_request(port, "GET", download_url, mismatch_hdrs);
    ASSERT_EQ(mismatch_get.status, 200);
    EXPECT_EQ(mismatch_get.body.size(), expected_size);

    auto multi_hdrs = hdrs;
    multi_hdrs.push_back({"Range", "bytes=0-3, 4-7"});
    EXPECT_EQ(send_http_request(port, "GET", download_url, multi_hdrs).status, 400);

    auto unsat_hdrs = hdrs;
    unsat_hdrs.push_back({"Range", "bytes=999999999-"});
    const auto unsat_get = send_http_request(port, "GET", download_url, unsat_hdrs);
    EXPECT_EQ(unsat_get.status, 416);
    EXPECT_EQ(unsat_get.header("content-range"), "bytes */" + std::to_string(expected_size));

    EXPECT_EQ(send_http_request(port, "GET", "/api/artifacts/..etc/files/f.1", hdrs).status, 400);

    // Mid-transfer client disconnect cleans up ReadLease & DownloadSession.
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        const std::string raw_req = "GET " + download_url +
                                    " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                    "\r\nAuthorization: Bearer " + bearer_tok +
                                    "\r\nRoDAQ-API-Version: 1.0\r\nConnection: close\r\n\r\n";
        (void)::send(fd, raw_req.data(), raw_req.size(), 0);
        char tiny[32];
        (void)::recv(fd, tiny, sizeof(tiny), 0);
        ::close(fd);
    }
    {
        const auto dl = std::chrono::steady_clock::now() + 2s;
        while (server.download_metrics().active > 0 && std::chrono::steady_clock::now() < dl) {
            std::this_thread::sleep_for(10ms);
        }
        EXPECT_EQ(server.download_metrics().active, 0u);
    }

    // 6. Configure, Reset, and Static SPA.
    auto cfg_hdrs = hdrs;
    cfg_hdrs.push_back({"Content-Type", "application/json"});
    cfg_hdrs.push_back({"Idempotency-Key", "cfg-1"});
    const auto cfg_resp = send_http_request(
        port, "PUT", "/api/tasks/" + task_id + "/config", cfg_hdrs,
        make_portable_task_json("http-task-renamed"));
    ASSERT_EQ(cfg_resp.status, 202) << cfg_resp.body;
    ASSERT_EQ(
        wait_op_http(nlohmann::json::parse(cfg_resp.body)["data"]["operation_id"]), "succeeded");

    auto reset_hdrs = hdrs;
    reset_hdrs.push_back({"Content-Type", "application/json"});
    reset_hdrs.push_back({"Idempotency-Key", "reset-1"});
    const auto reset_resp = send_http_request(
        port, "POST", "/api/tasks/" + task_id + "/operations", reset_hdrs,
        R"({"kind":"reset","parameters":{}})");
    ASSERT_EQ(reset_resp.status, 202);
    ASSERT_EQ(
        wait_op_http(nlohmann::json::parse(reset_resp.body)["data"]["operation_id"]), "succeeded");

    EXPECT_EQ(send_http_request(port, "GET", "/").status, 200);
    EXPECT_EQ(send_http_request(port, "GET", "/assets/app.hash123.js").status, 200);
    EXPECT_EQ(send_http_request(port, "GET", "/api/does-not-exist", hdrs).status, 404);

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    ASSERT_TRUE(server.shutdown(deadline).ok());
    ASSERT_TRUE(runtime.value()->shutdown(deadline).ok());
    shutdown_guard.done = true;
}

TEST(
    ServerHttpIntegrationTest,
    DaemonBinaryRestartResetsTasksToStoppedClearsOldOpsAndRebuildsCatalog) {
    const fs::path root =
        fs::temp_directory_path() /
        ("rodaq-daemon-e2e-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "packages");
    fs::create_directories(root / "state");
    fs::create_directories(root / "volume");
    fs::create_directories(root / "web/assets");
    struct Cleanup {
        fs::path r;
        ~Cleanup() {
            restore_writable_tree(r / "packages");
            std::error_code ec;
            fs::remove_all(r, ec);
        }
    } cleanup{root};

    fs::copy(
        fs::path(RODAQ_SERVER_STORAGE_PACKAGE) / "rodaq.fake.storage",
        root / "packages" / "rodaq.fake.storage", fs::copy_options::recursive);
    fs::copy(
        fs::path(RODAQ_SERVER_ACQUISITION_PACKAGE) / "rodaq.fake.acquisition",
        root / "packages" / "rodaq.fake.acquisition", fs::copy_options::recursive);
    set_readonly_tree(root / "packages");

    {
        std::ofstream(root / "web/index.html") << "<!doctype html><html><body>RoDAQ</body></html>";
        std::ofstream(root / "web/web_manifest.json")
            << R"({"web_build_id":"daemon-1","api_major":1,"api_minor_min":0})";
    }

    const std::string bearer_tok = "rodaq-daemon-bearer-secret-0123456789abcdef";
    const fs::path token_file = root / "server.token";
    {
        std::ofstream(token_file) << bearer_tok << "\n";
        ::chmod(token_file.c_str(), 0600);
    }

    const std::uint16_t port = pick_free_loopback_port();
    const fs::path dep_file = root / "deployment.json";
    auto write_deployment = [&](const std::string& open_mode) {
        std::ofstream(dep_file) << nlohmann::json{
            {"format_version", "1.0"},
            {"static_assets_root", (root / "web").string()},
            {"server",
             {{"format_version", "1.0"},
              {"http", {{"listen_address", "127.0.0.1"}, {"port", port}, {"worker_threads", 2}}},
              {"security", {{"bearer_token_file", token_file.string()}}}}},
            {"daq",
             {{"state_root", (root / "state").string()},
              {"volume_root", (root / "volume").string()},
              {"mode", open_mode},
              {"plugin_search_paths", {(root / "packages").string()}},
              {"qualified_runtime_profile_id", "daemon-e2e"},
              {"max_tasks", 2},
              {"max_pipelines_per_task", 2},
              {"volume", {{"name", "local"}}},
              {"profiles",
               {{{"name", "default"},
                 {"payload_pool_bytes", 65536},
                 {"max_payload_bytes", 4096},
                 {"max_logical_bytes", 65536},
                 {"max_segment_physical_bytes", 1048576},
                 {"max_descriptors", 16},
                 {"max_prepared_descriptors", 16},
                 {"max_streams", 8}}}},
              {"allowed_backends",
               {{{"plugin_id", "rodaq.fake.acquisition"},
                 {"plugin_version", "1.0.0"},
                 {"role", "acquisition"},
                 {"support_status", "qualified_candidate"},
                 {"qualification_status", "qualified"}},
                {{"plugin_id", "rodaq.fake.storage"},
                 {"plugin_version", "1.0.0"},
                 {"role", "storage"},
                 {"storage_format", "fake"},
                 {"support_status", "required"},
                 {"qualification_status",
                  "qualified"}}}}}}}.dump(2);
    };

    const std::vector<std::pair<std::string, std::string>> hdrs = {
        {"Authorization", "Bearer " + bearer_tok}, {"RoDAQ-API-Version", "1.0"}};

    auto spawn_daemon = [&]() -> pid_t {
        const pid_t pid = ::fork();
        if (pid == 0) {
            std::string bin = RODAQ_SERVER_BINARY;
            std::string flag = "--config";
            std::string arg = dep_file.string();
            char* argv[] = {bin.data(), flag.data(), arg.data(), nullptr};
            ::execv(bin.c_str(), argv);
            ::_exit(127);
        }
        return pid;
    };

    auto wait_ready = [&]() -> bool {
        const auto dl = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < dl) {
            const auto r = send_http_request(port, "GET", "/api/system", hdrs);
            if (r.status == 200) {
                const auto j = nlohmann::json::parse(r.body, nullptr, false);
                if (!j.is_discarded() && j["data"]["server_phase"] == "ready") {
                    return true;
                }
            }
            std::this_thread::sleep_for(30ms);
        }
        return false;
    };

    auto stop_daemon = [&](pid_t pid) -> int {
        ::kill(pid, SIGTERM);
        int status = 0;
        const auto dl = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < dl) {
            const pid_t got = ::waitpid(pid, &status, WNOHANG);
            if (got == pid) {
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            std::this_thread::sleep_for(20ms);
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        return -2;
    };

    struct ChildGuard {
        pid_t pid{-1};
        ~ChildGuard() {
            if (pid > 0) {
                ::kill(pid, SIGKILL);
                int st = 0;
                ::waitpid(pid, &st, 0);
            }
        }
    };

    write_deployment("initialize");
    ChildGuard child1{spawn_daemon()};
    ASSERT_GT(child1.pid, 0);
    ASSERT_TRUE(wait_ready());

    auto create_hdrs = hdrs;
    create_hdrs.push_back({"Content-Type", "application/json"});
    create_hdrs.push_back({"Idempotency-Key", "daemon-create-1"});
    const auto created = send_http_request(
        port, "POST", "/api/tasks", create_hdrs, make_portable_task_json("persisted-task"));
    ASSERT_EQ(created.status, 201) << created.body;
    const std::string task_id = nlohmann::json::parse(created.body)["data"]["task_id"];

    auto start_hdrs = hdrs;
    start_hdrs.push_back({"Content-Type", "application/json"});
    start_hdrs.push_back({"Idempotency-Key", "daemon-start-1"});
    const auto st_resp = send_http_request(
        port, "POST", "/api/tasks/" + task_id + "/operations", start_hdrs,
        R"({"kind":"start","parameters":{}})");
    ASSERT_EQ(st_resp.status, 202);
    const std::string start_op = nlohmann::json::parse(st_resp.body)["data"]["operation_id"];

    auto wait_op = [&](const std::string& op_id, std::string& last_body) -> std::string {
        const auto dl = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < dl) {
            const auto r = send_http_request(
                port, "GET", "/api/tasks/" + task_id + "/operations/" + op_id, hdrs);
            if (r.status == 200) {
                last_body = r.body;
                const std::string s = nlohmann::json::parse(r.body)["data"]["state"];
                if (s == "succeeded" || s == "failed") {
                    return s;
                }
            }
            std::this_thread::sleep_for(25ms);
        }
        return "timeout";
    };
    std::string op_body;
    ASSERT_EQ(wait_op(start_op, op_body), "succeeded") << op_body;
    std::this_thread::sleep_for(120ms);

    auto stop_hdrs = hdrs;
    stop_hdrs.push_back({"Content-Type", "application/json"});
    stop_hdrs.push_back({"Idempotency-Key", "daemon-stop-1"});
    const auto sp_resp = send_http_request(
        port, "POST", "/api/tasks/" + task_id + "/operations", stop_hdrs,
        R"({"kind":"stop","parameters":{}})");
    ASSERT_EQ(sp_resp.status, 202);
    ASSERT_EQ(
        wait_op(nlohmann::json::parse(sp_resp.body)["data"]["operation_id"], op_body), "succeeded");

    const auto arts1 = send_http_request(port, "GET", "/api/artifacts?task_id=" + task_id, hdrs);
    ASSERT_EQ(arts1.status, 200);
    const auto items1 = nlohmann::json::parse(arts1.body)["data"]["items"];
    ASSERT_FALSE(items1.empty());
    const std::string art_id = items1[0]["artifact_id"];

    const int rc1 = stop_daemon(child1.pid);
    child1.pid = -1;
    EXPECT_EQ(rc1, 0);

    // Run #2: Existing mode -> Task=Stopped/Idle, old Operation=404, Artifact Catalog rebuilt!
    write_deployment("existing");
    ChildGuard child2{spawn_daemon()};
    ASSERT_GT(child2.pid, 0);
    ASSERT_TRUE(wait_ready());

    const auto persisted_task = send_http_request(port, "GET", "/api/tasks/" + task_id, hdrs);
    ASSERT_EQ(persisted_task.status, 200);
    const auto task_data = nlohmann::json::parse(persisted_task.body)["data"];
    EXPECT_EQ(task_data["name"], "persisted-task");
    EXPECT_EQ(task_data["desired_state"], "stopped");
    EXPECT_EQ(task_data["execution_condition"], "idle");

    // Old in-memory operation ID from previous daemon returns 404 operation_not_found_or_expired.
    const auto old_op =
        send_http_request(port, "GET", "/api/tasks/" + task_id + "/operations/" + start_op, hdrs);
    EXPECT_EQ(old_op.status, 404);
    EXPECT_EQ(
        nlohmann::json::parse(old_op.body)["error"]["code"], "operation_not_found_or_expired");

    const auto persisted_art = send_http_request(port, "GET", "/api/artifacts/" + art_id, hdrs);
    ASSERT_EQ(persisted_art.status, 200);
    const std::string dl_url =
        nlohmann::json::parse(persisted_art.body)["data"]["files"][0]["download_url"];
    const auto downloaded = send_http_request(port, "GET", dl_url, hdrs);
    ASSERT_EQ(downloaded.status, 200);
    EXPECT_GT(downloaded.body.size(), 16u);

    const int rc2 = stop_daemon(child2.pid);
    child2.pid = -1;
    EXPECT_EQ(rc2, 0);
}

}  // namespace
