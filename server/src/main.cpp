#include "daq/startup.hpp"
#include "security/auth.hpp"
#include "server_app.hpp"
#include "server_config.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <rodaq/logging.hpp>

namespace {

namespace fs = std::filesystem;
namespace daq = rodaq::daq;
namespace srv = rodaq::server;
using NlJson = nlohmann::json;
using namespace std::chrono_literals;

std::atomic<bool> g_shutdown_requested{false};

void handle_signal(int /*sig*/) noexcept {
    g_shutdown_requested.store(true, std::memory_order_release);
}

daq::ResourceProfile make_default_profile(const std::string& name) {
    daq::ResourceProfile profile;
    profile.name = name;
    profile.instance.records = 256;
    profile.instance.logical_bytes = 4ull * 1024 * 1024;
    profile.instance.payloads = {16ull * 1024 * 1024, 65536, 1024 * 1024, 256};
    profile.instance.queue_capacity = 128;
    profile.instance.callback_concurrency = 4;
    profile.storage.header.struct_size = sizeof(rodaq::storage::PrepareConfig);
    profile.storage.max_record_bytes = 1024 * 1024;
    profile.storage.max_batch_bytes = 4ull * 1024 * 1024;
    profile.storage.max_batch_records = 64;
    profile.storage.max_descriptors_per_segment = 64;
    profile.storage.max_segment_records = 100000;
    profile.storage.max_segment_input_bytes = 128ull * 1024 * 1024;
    profile.storage.max_segment_physical_bytes = 256ull * 1024 * 1024;
    profile.storage.max_segment_file_count = 16;
    profile.storage.max_relative_path_bytes = 128;
    return profile;
}

rodaq::Result<daq::StartupOptions> parse_deployment_options(const NlJson& doc, bool force_init) {
    if (!doc.contains("deployment") || !doc["deployment"].is_object()) {
        return rodaq::Status(
            rodaq::StatusCode::InvalidArgument,
            "deployment.json missing required 'deployment' object");
    }
    const auto& dep = doc["deployment"];
    if (!dep.contains("state_root") || !dep["state_root"].is_string() ||
        !dep.contains("volume_root") || !dep["volume_root"].is_string()) {
        return rodaq::Status(
            rodaq::StatusCode::InvalidArgument, "deployment requires state_root and volume_root");
    }

    daq::StartupOptions opts;
    opts.state_root = dep["state_root"].get<std::string>();
    opts.volume_root = dep["volume_root"].get<std::string>();

    const std::string open_mode =
        force_init ? "initialize" : dep.value("open_mode", std::string("auto"));
    if (open_mode == "initialize") {
        opts.mode = daq::DeploymentOpenMode::Initialize;
    } else if (open_mode == "existing") {
        opts.mode = daq::DeploymentOpenMode::Existing;
    } else if (open_mode == "auto") {
        opts.mode = fs::is_regular_file(opts.state_root / "bootstrap.json")
                        ? daq::DeploymentOpenMode::Existing
                        : daq::DeploymentOpenMode::Initialize;
    } else {
        return rodaq::Status(
            rodaq::StatusCode::InvalidArgument,
            "deployment.open_mode must be auto|initialize|existing");
    }

    if (opts.mode == daq::DeploymentOpenMode::Initialize) {
        std::error_code ec;
        fs::create_directories(opts.state_root, ec);
        if (ec) {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "Failed to create state_root");
        }
        fs::create_directories(opts.volume_root, ec);
        if (ec) {
            return rodaq::Status(rodaq::StatusCode::Unavailable, "Failed to create volume_root");
        }
    }

    if (dep.contains("plugin_search_paths") && dep["plugin_search_paths"].is_array()) {
        for (const auto& p : dep["plugin_search_paths"]) {
            if (p.is_string()) {
                opts.plugins.search_paths.emplace_back(p.get<std::string>());
            }
        }
    }
    opts.plugins.max_loaded_plugins = dep.value("max_loaded_plugins", 16u);
    opts.plugins.max_instances = dep.value("max_instances", 64u);
    opts.plugins.max_config_size = dep.value("max_config_size", 65536u);
    opts.plugins.shutdown_timeout = 5s;

    opts.capacity.max_managed_bytes = dep.value("max_managed_bytes", 16ull * 1024 * 1024 * 1024);
    opts.capacity.max_commit_bytes = dep.value("max_commit_bytes", 512ull * 1024 * 1024);
    opts.capacity.stop_free_bytes = dep.value("stop_free_bytes", 64ull * 1024 * 1024);
    opts.capacity.low_free_bytes = dep.value("low_free_bytes", 128ull * 1024 * 1024);
    opts.capacity.recovery_free_bytes = dep.value("recovery_free_bytes", 256ull * 1024 * 1024);
    opts.capacity.recovery_stable_ns = 1'000'000'000ull;
    opts.capacity.space_sample_max_age_ns = 60'000'000'000ull;
    opts.ledger_records = dep.value("ledger_records", 256u);

    opts.settings.deployment_generation = dep.value("deployment_generation", 1ull);
    opts.settings.qualified_runtime_profile_id =
        dep.value("qualified_runtime_profile_id", std::string("rodaq-v1-local"));
    opts.settings.output_root = dep.value("output_root", std::string("local"));
    opts.settings.max_tasks = dep.value("max_tasks", 4u);
    opts.settings.artifact_audit_window = std::chrono::hours{24};
    opts.settings.profiles.push_back(make_default_profile("basic"));

    if (dep.contains("allowed_plugins") && dep["allowed_plugins"].is_array() &&
        !dep["allowed_plugins"].empty()) {
        for (const auto& item : dep["allowed_plugins"]) {
            if (!item.is_object() || !item.contains("plugin_id") ||
                !item.contains("plugin_version") || !item.contains("role")) {
                return rodaq::Status(
                    rodaq::StatusCode::InvalidArgument, "Invalid allowed_plugins entry");
            }
            const auto id = item["plugin_id"].get<std::string>();
            const auto ver = item["plugin_version"].get<std::string>();
            const auto role_s = item["role"].get<std::string>();
            const auto role = role_s == "acquisition" ? rodaq::task::AdmissionRole::Acquisition
                                                      : rodaq::task::AdmissionRole::Storage;
            opts.settings.allowed_plugins.push_back({id, ver, role});
            if (role == rodaq::task::AdmissionRole::Storage) {
                opts.settings.storage.push_back(
                    {id, ver, item.value("format", std::string("mcap"))});
            }
        }
    } else {
        opts.settings.allowed_plugins = {
            {"com.rodaq.acquisition.ros2", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"com.rodaq.storage.mcap", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        opts.settings.storage = {{"com.rodaq.storage.mcap", "1.0.0", "mcap"}};
    }

    return opts;
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "/etc/rodaq/deployment.json";
    bool force_init = false;
    bool validate_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--init") {
            force_init = true;
        } else if (arg == "--validate-only") {
            validate_only = true;
        } else if (arg == "--hash-bearer" && i + 1 < argc) {
            auto hashed = srv::compute_bearer_verifier(argv[++i]);
            if (!hashed.ok()) {
                std::cerr << "rodaq: failed to hash bearer token\n";
                return 1;
            }
            std::cout << hashed.value() << "\n";
            return 0;
        } else if (arg == "--version") {
            std::cout << "rodaq 0.1.0\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: rodaq --config <deployment.json> [--init] [--validate-only] "
                         "[--hash-bearer <token>] [--version]\n";
            return 0;
        }
    }

    std::ifstream in(config_path);
    if (!in.good()) {
        std::cerr << "rodaq: failed to open deployment config: " << config_path << "\n";
        return 2;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    try {
        const auto doc = NlJson::parse(ss.str());
        if (!doc.is_object() || !doc.contains("server")) {
            std::cerr << "rodaq: deployment.json missing required 'server' object\n";
            return 2;
        }
        auto server_cfg = srv::parse_server_config(doc["server"].dump());
        if (!server_cfg.ok()) {
            std::cerr << "rodaq: invalid server config: " << server_cfg.status().message() << "\n";
            return 2;
        }

        // Default static web asset root to <exe_dir>/../share/rodaq/web if not overridden.
        if (server_cfg.value().static_asset_root.empty()) {
            std::error_code ec;
            const auto exe = fs::canonical("/proc/self/exe", ec);
            if (!ec) {
                const auto candidate = exe.parent_path().parent_path() / "share/rodaq/web";
                if (fs::is_regular_file(candidate / "index.html")) {
                    server_cfg.value().static_asset_root = candidate;
                }
            }
        }

        auto dep_opts = parse_deployment_options(doc, force_init);
        if (!dep_opts.ok()) {
            std::cerr << "rodaq: invalid deployment config: " << dep_opts.status().message()
                      << "\n";
            return 2;
        }
        if (validate_only) {
            std::cout << "rodaq: configuration valid (" << server_cfg.value().http.listen_address
                      << ":" << server_cfg.value().http.port << ")\n";
            return 0;
        }

        std::signal(SIGTERM, handle_signal);
        std::signal(SIGINT, handle_signal);

        auto composition = daq::Composition::open(std::move(dep_opts.value()));
        if (!composition.ok()) {
            std::cerr << "rodaq: failed to open Core composition (status="
                      << static_cast<int>(composition.status().code())
                      << "): " << composition.status().message() << "\n";
            return 1;
        }

        auto* comp_ptr = composition.value().get();
        srv::ServerServices services;
        services.tasks = &comp_ptr->tasks();
        services.system = &comp_ptr->system();
        services.artifacts = &comp_ptr->artifacts();
        services.core_shutdown = [comp_ptr](auto dl) { return comp_ptr->shutdown(dl); };

        const auto shutdown_timeout = server_cfg.value().timeouts.shutdown;
        const auto listen_url = "http://" + server_cfg.value().http.listen_address + ":" +
                                std::to_string(server_cfg.value().http.port);

        srv::ServerApp server;
        auto started = server.start(std::move(server_cfg.value()), std::move(services), true);
        if (!started.ok()) {
            std::cerr << "rodaq: failed to start HTTP server: " << started.status().message()
                      << "\n";
            (void)comp_ptr->shutdown(std::chrono::steady_clock::now() + shutdown_timeout);
            return 1;
        }

        std::cout << "RoDAQ V1 running at " << listen_url << "\n";
        if (server.web_manifest().usable) {
            std::cout << "Web console active (" << server.web_manifest().web_build_id << ")\n";
        } else if (server.web_manifest().checked) {
            std::cerr << "rodaq: warning: web console unavailable: "
                      << server.web_manifest().error.value_or("unknown") << "\n";
        }
        std::cout.flush();

        while (!g_shutdown_requested.load(std::memory_order_acquire)) {
            (void)comp_ptr->observe();
            for (int i = 0; i < 10 && !g_shutdown_requested.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(50ms);
            }
        }

        std::cout << "rodaq: shutting down gracefully...\n";
        auto stopped = server.shutdown(std::chrono::steady_clock::now() + shutdown_timeout);
        if (!stopped.ok()) {
            std::cerr << "rodaq: shutdown returned status="
                      << static_cast<int>(stopped.status().code()) << "\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "rodaq: fatal startup error: " << ex.what() << "\n";
        return 2;
    }
}
