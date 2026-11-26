#pragma once

#include "api/error_response.hpp"
#include "config.hpp"
#include "download/download_registry.hpp"
#include "security/auth.hpp"
#include "service_call_executor.hpp"
#include "static_assets.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/result.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

namespace rodaq::server {

struct ServerServices {
    TaskService* tasks{nullptr};
    SystemService* system{nullptr};
    ArtifactService* artifacts{nullptr};
};

// Process-level HTTP server application owning Drogon listener lifecycle,
// AuthState, bounded ServiceCallExecutor, DownloadRegistry, and StaticAssetService.
// Never owns Core domain state; only references formal public service facades.
class ServerApp {
public:
    ServerApp();
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;
    ~ServerApp() noexcept;

    // Starts the loopback Drogon HTTP listener. When initial_phase is Recovering,
    // only /api/session and GET /api/system are served until mark_ready() is called.
    Result<void> start(
        ServerConfig config, ServerServices services,
        ServerPhase initial_phase = ServerPhase::Ready);

    // Atomically installs ready Core services and transitions Recovering -> Ready.
    Result<void> mark_ready(ServerServices services);

    // Explicit ordered shutdown: transitions to Draining, cancels active
    // downloads and unstarted queued work, waits bounded settlement, stops Drogon.
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

    ServerPhase phase() const noexcept;

    ExecutorMetrics executor_metrics() const;

    DownloadMetrics download_metrics() const;

    std::size_t active_sessions() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq::server
