#pragma once

#include "config.hpp"
#include "download/download_registry.hpp"
#include "service_call_executor.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/result.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

namespace rodaq::server {

// Shared startup/exit latch owned by ServerApp and Drogon's BeginningAdvice /
// thread wrapper so a late startup callback after timeout never dereferences a
// destroyed stack frame.
struct ServerStartupLatch {
    std::mutex mutex;
    std::condition_variable cv;
    bool loop_running{false};
    bool loop_exited{false};

    void mark_running() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            loop_running = true;
        }
        cv.notify_all();
    }

    void mark_exited() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex);
            loop_exited = true;
        }
        cv.notify_all();
    }

    bool wait_running_for(std::chrono::nanoseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return loop_running || loop_exited; }) &&
               loop_running;
    }

    bool wait_exited_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_until(lock, deadline, [&] { return loop_exited; });
    }
};

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

    // True when no Drogon event loop thread or Server worker threads remain active.
    // Callers must not tear down Core while quiescent() is false.
    bool quiescent() const noexcept;

    ServerPhase phase() const noexcept;

    ControlMetrics control_metrics() const;

    DownloadMetrics download_metrics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq::server
