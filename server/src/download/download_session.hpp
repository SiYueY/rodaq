#pragma once

#include "../config.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <trantor/net/AsyncStream.h>
#include <trantor/net/TcpConnection.h>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/result.hpp>

namespace rodaq::server {

enum class RangeParseStatus { Ok, NoRange, Malformed, MultipleRanges, Unsatisfiable };

struct ByteRange {
    std::uint64_t start{0};
    std::uint64_t end{0};  // Inclusive byte index when length > 0.
    std::uint64_t length{0};
    bool partial{false};
};

struct RangeParseResult {
    RangeParseStatus status{RangeParseStatus::NoRange};
    ByteRange range;
};

RangeParseResult parse_single_byte_range(
    std::string_view range_header, std::uint64_t full_length) noexcept;

// Strong ETag comparison for single-value If-Range header.
bool if_range_matches_etag(std::string_view if_range_header, std::string_view strong_etag) noexcept;

std::string sanitize_download_filename(std::string_view raw_display_name);

std::string build_content_disposition(std::string_view raw_display_name);

class DownloadRegistry;

// Unique cross-thread ownership bridge between ArtifactReadHandle (blocking
// disk worker) and Drogon/Trantor AsyncStream transport (event loop). Releases
// ArtifactReadHandle/ReadLease and download capacity strictly once, only after
// any in-flight read_at() has actually returned and transport has closed.
class DownloadSession : public std::enable_shared_from_this<DownloadSession> {
public:
    struct Options {
        std::uint64_t session_id{0};
        std::uint64_t chunk_bytes{1048576};
        std::uint64_t max_bytes_in_flight{8388608};
        ByteRange range;
        std::shared_ptr<ArtifactReadHandle> handle;
        std::weak_ptr<trantor::TcpConnection> connection;
        std::function<bool(std::shared_ptr<DownloadSession>, std::uint64_t, std::size_t)>
            submit_disk_read;
        std::function<void(std::uint64_t session_id, std::uint64_t bytes_sent, bool cancelled)>
            on_terminal;
    };

    static std::shared_ptr<DownloadSession> create(Options options);

    DownloadSession(const DownloadSession&) = delete;
    DownloadSession& operator=(const DownloadSession&) = delete;
    ~DownloadSession() noexcept;

    std::uint64_t id() const noexcept { return session_id_; }

    void attach_and_start(trantor::AsyncStreamPtr stream, std::string initial_header_bytes = {});

    void request_cancel() noexcept;

    void on_transport_writable() noexcept;

    void execute_disk_read(std::uint64_t offset, std::size_t to_read);

    bool is_settled() const noexcept;

private:
    struct TerminalInvocation {
        std::function<void(std::uint64_t, std::uint64_t, bool)> callback;
        std::uint64_t session_id{0};
        std::uint64_t bytes_sent{0};
        bool cancelled{false};
    };

    explicit DownloadSession(Options options);

    std::optional<TerminalInvocation> pump_locked() noexcept;
    std::optional<TerminalInvocation> schedule_read_locked() noexcept;
    std::optional<TerminalInvocation> try_settle_locked() noexcept;
    void close_stream_locked() noexcept;
    static void invoke_terminal(std::optional<TerminalInvocation> invocation) noexcept;

    const std::uint64_t session_id_;
    const std::uint64_t chunk_bytes_;
    const std::uint64_t max_bytes_in_flight_;
    const ByteRange range_;
    std::weak_ptr<trantor::TcpConnection> connection_;
    std::function<bool(std::shared_ptr<DownloadSession>, std::uint64_t, std::size_t)>
        submit_disk_read_;
    std::function<void(std::uint64_t, std::uint64_t, bool)> on_terminal_;

    mutable std::mutex mutex_;
    std::shared_ptr<ArtifactReadHandle> handle_;
    trantor::AsyncStreamPtr stream_;
    std::vector<std::byte> chunk_buffer_;
    std::uint64_t next_offset_{0};
    std::uint64_t remaining_{0};
    std::uint64_t baseline_tcp_sent_{0};
    std::uint64_t total_queued_bytes_{0};
    std::uint64_t total_body_bytes_sent_{0};
    bool started_{false};
    bool inflight_read_{false};
    bool waiting_backpressure_{false};
    bool cancel_requested_{false};
    bool eof_or_error_{false};
    bool settled_{false};
};

}  // namespace rodaq::server
