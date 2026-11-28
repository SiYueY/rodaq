#include "config.hpp"
#include "server_app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <rodaq/daq/runtime.hpp>
#include <rodaq/logging.hpp>

namespace {

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) noexcept { g_stop_requested.store(true, std::memory_order_release); }

void print_usage() {
    std::cerr << "Usage: rodaq --config /absolute/path/to/deployment.json [--assets "
                 "/absolute/path/to/web]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path config_path;
    std::filesystem::path assets_override;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--config" || arg == "--deployment") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--assets" && i + 1 < argc) {
            assets_override = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage();
            return 2;
        }
    }
    if (config_path.empty()) {
        print_usage();
        return 2;
    }

    rodaq::logging::Config log_cfg;
    log_cfg.name = "rodaq";
    log_cfg.level = rodaq::logging::Level::Info;
    log_cfg.console = true;
    log_cfg.console_stderr = true;
    if (auto res = rodaq::logging::initialize(log_cfg); !res.ok()) {
        std::cerr << "Failed to initialize logging: " << res.status().message() << "\n";
        return 1;
    }

    auto dep = rodaq::server::load_deployment_config(config_path);
    if (!dep.ok()) {
        RODAQ_LOG_ERROR("deployment config error: {}", dep.status().message());
        (void)rodaq::logging::shutdown();
        return 1;
    }
    if (!assets_override.empty()) {
        if (!assets_override.is_absolute()) {
            RODAQ_LOG_ERROR("--assets must be an absolute path");
            (void)rodaq::logging::shutdown();
            return 2;
        }
        dep.value().server.static_assets_root = assets_override;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // Synchronous fail-fast startup: open Core + complete Artifact Catalog scan before HTTP Ready.
    auto runtime_res = rodaq::DaqRuntime::open(std::move(dep.value().daq));
    if (!runtime_res.ok()) {
        RODAQ_LOG_ERROR("DaqRuntime::open failed: {}", runtime_res.status().message());
        (void)rodaq::logging::shutdown();
        return 1;
    }
    auto runtime = std::move(runtime_res.value());
    (void)runtime->observe();

    rodaq::server::ServerApp server;
    rodaq::server::ServerServices services;
    services.tasks = &runtime->tasks();
    services.system = &runtime->system();
    services.artifacts = &runtime->artifacts();

    const auto shutdown_budget = dep.value().server.timeouts.shutdown;
    if (auto started = server.start(std::move(dep.value().server), services); !started.ok()) {
        RODAQ_LOG_ERROR("ServerApp::start failed: {}", started.status().message());
        (void)runtime->shutdown(std::chrono::steady_clock::now() + shutdown_budget);
        (void)rodaq::logging::shutdown();
        return 1;
    }

    std::atomic<bool> stop_observe{false};
    std::thread observe_thread([&] {
        while (!stop_observe.load(std::memory_order_acquire)) {
            (void)runtime->observe();
            for (int i = 0; i < 10 && !stop_observe.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }
    });

    while (!g_stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    RODAQ_LOG_INFO("signal received; initiating ordered shutdown");
    stop_observe.store(true, std::memory_order_release);
    if (observe_thread.joinable()) {
        observe_thread.join();
    }

    const auto deadline = std::chrono::steady_clock::now() + shutdown_budget;
    auto srv_sd = server.shutdown(deadline);
    auto core_sd = runtime->shutdown(deadline);
    if (!srv_sd.ok()) {
        RODAQ_LOG_ERROR("ServerApp::shutdown error: {}", srv_sd.status().message());
    }
    if (!core_sd.ok()) {
        RODAQ_LOG_ERROR("DaqRuntime::shutdown error: {}", core_sd.status().message());
    }
    (void)rodaq::logging::shutdown();
    return (srv_sd.ok() && core_sd.ok()) ? 0 : 1;
}
