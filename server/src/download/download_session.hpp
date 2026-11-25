#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <drogon/HttpResponse.h>
#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/result.hpp>

#include "../server_config.hpp"

namespace rodaq::server {

struct ByteRange {
    std::uint64_t start{0};
    std::uint64_t end{0};  // inclusive
    std::uint64_t length() const noexcept { return end >= start ? (end - start + 1) : 0; }
};

enum class RangeParseOutcome {
    None,
    Valid,
    Malformed,
    MultipleNotSupported,
    Unsatisfiable,
};

struct RangeParseResult {
    RangeParseOutcome outcome{RangeParseOutcome::None};
    ByteRange range;
};

RangeParseResult parse_single_byte_range(
    std::string_view header, std::uint64_t full_length) noexcept;

// Sanitizes a committed Artifact display name for Content-Disposition (no CR/LF, control, path
// sep).
std::string sanitize_display_filename(std::string_view raw);

class DownloadSession;

// Bounded active-download registry + dedicated disk-read worker pool.
class DownloadRegistry {
public:
    static Result<std::unique_ptr<DownloadRegistry>> create(const DownloadConfig& config);

    DownloadRegistry(const DownloadRegistry&) = delete;
    DownloadRegistry& operator=(const DownloadRegistry&) = delete;
    ~DownloadRegistry() noexcept;

    // Acquires a download slot and creates a DownloadSession owning `handle`.
    Result<std::shared_ptr<DownloadSession>> start_session(
        ArtifactReadHandle handle, ByteRange range, drogon::ResponseStreamPtr stream);

    void cancel_all() noexcept;
    Result<void> shutdown(std::chrono::steady_clock::time_point deadline);
    std::size_t active_downloads() const noexcept;

private:
    friend class DownloadSession;
    explicit DownloadRegistry(DownloadConfig config);
    void worker_loop() noexcept;
    void finish_session(const std::shared_ptr<DownloadSession>& session) noexcept;

    DownloadConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable empty_cv_;
    std::unordered_set<std::shared_ptr<DownloadSession>> active_;
    std::deque<std::shared_ptr<DownloadSession>> queue_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
    bool stopped_{false};
};

// Owns ArtifactReadHandle + bounded chunk buffer + Drogon ResponseStreamPtr off the event loop.
class DownloadSession : public std::enable_shared_from_this<DownloadSession> {
public:
    DownloadSession(
        DownloadRegistry* registry, ArtifactReadHandle handle, ByteRange range,
        std::uint64_t chunk_bytes, drogon::ResponseStreamPtr stream);

    void request_cancel() noexcept;
    bool cancelled() const noexcept { return cancel_requested_.load(std::memory_order_acquire); }

private:
    friend class DownloadRegistry;
    void run_on_worker() noexcept;

    DownloadRegistry* registry_;
    std::optional<ArtifactReadHandle> handle_;
    ByteRange range_;
    std::uint64_t chunk_bytes_;
    drogon::ResponseStreamPtr stream_;
    std::atomic<bool> cancel_requested_{false};
};

}  // namespace rodaq::server
