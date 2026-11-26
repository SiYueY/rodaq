#include "config.hpp"
#include "server_app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#include <rodaq/daq/runtime.hpp>
#include <rodaq/logging.hpp>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void handle_signal(int /*sig*/) noexcept {
    g_shutdown_requested.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path deployment_path = "/etc/rodaq/deployment.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if ((arg == "--deployment" || arg == "-c") && i + 1 < argc) {
            deployment_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: rodaq [--deployment <path>]\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }

    auto deployment = rodaq::server::load_deployment_config(deployment_path);
    if (!deployment.ok()) {
        std::cerr << "Failed to load deployment config: " << deployment.status().message() << "\n";
        return 1;
    }

    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);

    // 1. Start loopback HTTP listener in Recovering limited mode.
    rodaq::server::ServerApp server;
    auto started = server.start(
        deployment.value().server, rodaq::server::ServerServices{},
        rodaq::server::ServerPhase::Recovering);
    if (!started.ok()) {
        std::cerr << "Failed to start HTTP server: " << started.status().message() << "\n";
        return 1;
    }

    // 2. Initialize Core DaqRuntime and complete Boot Reconciliation.
    auto runtime = rodaq::DaqRuntime::open(std::move(deployment.value().daq));
    if (!runtime.ok()) {
        std::cerr << "Failed to open DAQ runtime (status="
                  << static_cast<int>(runtime.status().code()) << ")\n";
        (void)server.shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
        return 1;
    }

    // 3. Atomically transition Server to Ready.
    rodaq::server::ServerServices services{
        &runtime.value()->tasks(), &runtime.value()->system(), &runtime.value()->artifacts()};
    auto ready = server.mark_ready(services);
    if (!ready.ok()) {
        std::cerr << "Failed to transition server to Ready\n";
        (void)server.shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
        (void)runtime.value()->shutdown(
            std::chrono::steady_clock::now() + std::chrono::seconds{10});
        return 1;
    }

    // 4. Periodic bounded maintenance observe() loop until SIGTERM/SIGINT.
    const auto observe_interval = deployment.value().observe_interval;
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        (void)runtime.value()->observe();
        const auto wake_at = std::chrono::steady_clock::now() + observe_interval;
        while (!g_shutdown_requested.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < wake_at) {
            std::this_thread::sleep_for(std::chrono::milliseconds{25});
        }
    }

    // 5. Parallel ordered shutdown of Server HTTP/downloads and Core Process Drain.
    const auto deadline =
        std::chrono::steady_clock::now() + deployment.value().server.timeouts.shutdown;
    auto core_shutdown =
        std::async(std::launch::async, [&] { return runtime.value()->shutdown(deadline); });
    const auto srv_res = server.shutdown(deadline);
    const auto core_res = core_shutdown.get();
    if (!srv_res.ok() || !core_res.ok()) {
        RODAQ_LOG_ERROR("shutdown did not settle cleanly within deadline");
        return 1;
    }
    return 0;
}
