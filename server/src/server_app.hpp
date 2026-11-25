#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/result.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

#include "api/dto.hpp"
#include "download/download_session.hpp"
#include "security/auth.hpp"
#include "server_config.hpp"
#include "service_call_executor.hpp"

namespace rodaq::server {

struct ServerServices {
    TaskService* tasks{nullptr};
    SystemService* system{nullptr};
    ArtifactService* artifacts{nullptr};
    // Optional maintenance hook called periodically or on shutdown.
    std::function<Result<void>(std::chrono::steady_clock::time_point)> core_shutdown;
};

struct WebManifestStatus {
    bool checked{false};
    bool usable{false};
    std::string web_build_id;
    std::optional<std::string> error;
};

class ServerApp {
public:
    ServerApp();
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;
    ~ServerApp() noexcept;

    // Starts Drogon listener in Recovering or Ready phase (if services are ready).
    Result<void> start(ServerConfig config, ServerServices services, bool start_ready = true);

    // Atomically promotes Recovering -> Ready after Core boot reconciliation barrier.
    void mark_ready() noexcept;
    void mark_failed() noexcept;

    // Controlled shutdown: stops accepting new mutations/downloads, cancels active downloads,
    // drains Drogon + Core in parallel, and settles executors.
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

    ServerPhase phase() const noexcept { return phase_.load(std::memory_order_acquire); }
    std::uint16_t bound_port() const noexcept { return bound_port_; }
    const ServerConfig& config() const noexcept { return config_; }
    const WebManifestStatus& web_manifest() const noexcept { return web_manifest_; }
    ExecutorMetrics executor_metrics() const noexcept;
    std::size_t active_sessions() const noexcept;
    std::size_t active_downloads() const noexcept;

private:
    struct Impl;
    void register_routes();
    void check_web_manifest();

    ServerConfig config_;
    ServerServices services_;
    std::atomic<ServerPhase> phase_{ServerPhase::Recovering};
    std::uint16_t bound_port_{0};
    WebManifestStatus web_manifest_;

    std::unique_ptr<AuthState> auth_;
    std::unique_ptr<ServiceCallExecutor> executor_;
    std::unique_ptr<DownloadRegistry> downloads_;
    std::thread drogon_thread_;
    bool started_{false};
    bool stopped_{false};
};

}  // namespace rodaq::server
