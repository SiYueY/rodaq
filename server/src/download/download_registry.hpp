#pragma once

#include "../config.hpp"
#include "download_session.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <trantor/net/TcpConnection.h>

#include <rodaq/result.hpp>

namespace rodaq::server {

enum class DownloadAdmissionFailure { GlobalSaturation, ShuttingDown };

struct DownloadMetrics {
    std::uint64_t active{0};
    std::uint64_t high_water_active{0};
    std::uint64_t total_started{0};
    std::uint64_t total_completed{0};
    std::uint64_t total_cancelled{0};
    std::uint64_t total_bytes_sent{0};
    std::uint64_t rejected_global{0};
};

struct DownloadAdmitResult {
    bool admitted{false};
    DownloadAdmissionFailure failure{DownloadAdmissionFailure::GlobalSaturation};
    std::shared_ptr<DownloadSession> session;
};

// Owns bounded download worker pool and active DownloadSession registry for
// capacity enforcement, disconnect cancel propagation, and ordered shutdown.
class DownloadRegistry {
public:
    static Result<std::unique_ptr<DownloadRegistry>> create(const DownloadConfig& config);

    DownloadRegistry(const DownloadRegistry&) = delete;
    DownloadRegistry& operator=(const DownloadRegistry&) = delete;
    ~DownloadRegistry() noexcept;

    // Runs bounded artifact file open / ReadLease acquisition on the download
    // worker pool so filesystem open never runs on the Drogon event loop.
    bool submit_open(std::function<void()> work);

    DownloadAdmitResult create_session(
        ByteRange range, std::shared_ptr<ArtifactReadHandle> handle,
        std::weak_ptr<trantor::TcpConnection> connection);

    void notify_connection_closed(const trantor::TcpConnection* raw_conn) noexcept;

    void cancel_all() noexcept;

    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);

    std::uint64_t active_count() const;

    DownloadMetrics metrics() const;

private:
    struct ReadJob {
        std::shared_ptr<DownloadSession> session;
        std::uint64_t offset{0};
        std::size_t to_read{0};
    };

    struct SharedState {
        // Acquired only before registry_mutex to keep cancel paths strictly noexcept
        // using pre-allocated cancel_scratch slots.
        std::mutex cancel_mutex;
        mutable std::mutex registry_mutex;
        std::condition_variable settled_cv;
        std::uint64_t next_session_id{1};
        std::size_t reserved_sessions{0};
        std::unordered_map<std::uint64_t, std::shared_ptr<DownloadSession>> active_sessions;
        std::unordered_map<std::uint64_t, const trantor::TcpConnection*> session_conns;
        std::unordered_map<const trantor::TcpConnection*, std::vector<std::uint64_t>> conn_sessions;
        std::vector<std::shared_ptr<DownloadSession>> cancel_scratch;
        DownloadMetrics metrics;
        bool stopping{false};

        std::mutex worker_mutex;
        std::condition_variable worker_cv;
        std::condition_variable open_settled_cv;
        std::vector<ReadJob> read_ring;
        std::size_t ring_head{0};
        std::size_t ring_count{0};
        std::deque<std::function<void()>> open_queue;
        std::size_t max_open_queue{16};
        std::size_t active_opens{0};
        bool workers_stopping{false};
    };

    explicit DownloadRegistry(DownloadConfig config);
    static void worker_loop(const std::shared_ptr<SharedState>& state);
    static bool submit_disk_read(
        const std::shared_ptr<SharedState>& state, std::shared_ptr<DownloadSession> session,
        std::uint64_t offset, std::size_t to_read) noexcept;
    static void on_session_terminal(
        const std::shared_ptr<SharedState>& state, std::uint64_t session_id,
        std::uint64_t bytes_sent, bool cancelled);

    DownloadConfig config_;
    std::shared_ptr<SharedState> state_;
    bool stopped_{false};
    std::vector<std::thread> workers_;
};

}  // namespace rodaq::server
