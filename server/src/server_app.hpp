#pragma once

#include "config.hpp"
#include "download/download_registry.hpp"
#include "service_call_executor.hpp"

#include <chrono>
#include <memory>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/result.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

namespace rodaq::server {

// Public Core service pointers injected into ServerApp, plus an optional shared
// lifetime pin keeping the Core owner alive through any detached late-returning
// worker after a shutdown timeout.
struct ServerServices {
    TaskService* tasks{nullptr};
    SystemService* system{nullptr};
    ArtifactService* artifacts{nullptr};
    std::shared_ptr<void> lifetime_pin;
};

// Owns Drogon HTTP listener, BearerAuthenticator, ControlExecutor,
// DownloadRegistry, StaticAssetService, and ordered shutdown.
class ServerApp {
public:
    ServerApp();
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;
    ~ServerApp() noexcept;

    // Starts loopback listener directly in Ready once Core + Catalog are ready.
    Result<void> start(ServerConfig config, ServerServices services);

    // Drains downloads and in-flight control calls within deadline, then stops Drogon.
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

    ServerPhase phase() const noexcept;

    ControlMetrics control_metrics() const;

    DownloadMetrics download_metrics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq::server
