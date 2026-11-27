#include "download_session.hpp"

#include "../api/json_codec.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace rodaq::server {
namespace {

std::string_view trim_ows(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string percent_encode_rfc5987(std::string_view input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (const char ch : input) {
        const auto c = static_cast<unsigned char>(ch);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                                c == '~';
        if (unreserved) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0Fu]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    return out;
}

}  // namespace

RangeParseResult parse_single_byte_range(
    std::string_view range_header, std::uint64_t full_length) noexcept {
    const auto trimmed = trim_ows(range_header);
    if (trimmed.empty()) {
        return {
            RangeParseStatus::NoRange,
            {0, full_length > 0 ? full_length - 1 : 0, full_length, false}};
    }
    constexpr std::string_view kPrefix = "bytes=";
    if (trimmed.size() <= kPrefix.size() || trimmed.substr(0, kPrefix.size()) != kPrefix) {
        return {RangeParseStatus::Malformed, {}};
    }
    const auto spec = trim_ows(trimmed.substr(kPrefix.size()));
    if (spec.empty()) {
        return {RangeParseStatus::Malformed, {}};
    }
    if (spec.find(',') != std::string_view::npos) {
        return {RangeParseStatus::MultipleRanges, {}};
    }
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos || spec.find('-', dash + 1) != std::string_view::npos) {
        return {RangeParseStatus::Malformed, {}};
    }
    const auto left = spec.substr(0, dash);
    const auto right = spec.substr(dash + 1);
    if (left.empty() && right.empty()) {
        return {RangeParseStatus::Malformed, {}};
    }

    if (left.empty()) {
        // Suffix range: bytes=-suffix_length
        const auto suffix = parse_u64_decimal(right);
        if (!suffix) {
            return {RangeParseStatus::Malformed, {}};
        }
        if (*suffix == 0 || full_length == 0) {
            return {RangeParseStatus::Unsatisfiable, {}};
        }
        const std::uint64_t len = std::min(*suffix, full_length);
        const std::uint64_t start = full_length - len;
        return {RangeParseStatus::Ok, {start, full_length - 1, len, true}};
    }

    const auto start = parse_u64_decimal(left);
    if (!start) {
        return {RangeParseStatus::Malformed, {}};
    }
    if (right.empty()) {
        // Open-ended range: bytes=start-
        if (full_length == 0 || *start >= full_length) {
            return {RangeParseStatus::Unsatisfiable, {}};
        }
        const std::uint64_t end = full_length - 1;
        return {RangeParseStatus::Ok, {*start, end, end - *start + 1, true}};
    }

    // Closed range: bytes=start-end
    const auto end_req = parse_u64_decimal(right);
    if (!end_req || *end_req < *start) {
        return {RangeParseStatus::Malformed, {}};
    }
    if (full_length == 0 || *start >= full_length) {
        return {RangeParseStatus::Unsatisfiable, {}};
    }
    const std::uint64_t end = std::min(*end_req, full_length - 1);
    return {RangeParseStatus::Ok, {*start, end, end - *start + 1, true}};
}

bool if_range_matches_etag(
    std::string_view if_range_header, std::string_view strong_etag) noexcept {
    const auto trimmed = trim_ows(if_range_header);
    if (trimmed.empty()) {
        return true;
    }
    // Weak ETags never match If-Range per RFC 9110 §13.1.5.
    if (trimmed.size() >= 2 && trimmed[0] == 'W' && trimmed[1] == '/') {
        return false;
    }
    if (!strong_etag.empty() && strong_etag.size() >= 2 && strong_etag.front() == '"' &&
        strong_etag.back() == '"') {
        return trimmed == strong_etag;
    }
    return trimmed.size() == strong_etag.size() + 2 && trimmed.front() == '"' &&
           trimmed.back() == '"' && trimmed.substr(1, strong_etag.size()) == strong_etag;
}

std::string sanitize_download_filename(std::string_view raw_display_name) {
    std::string out;
    out.reserve(std::min<std::size_t>(raw_display_name.size(), 128));
    for (const char ch : raw_display_name) {
        if (out.size() >= 128) {
            break;
        }
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7F || ch == '/' || ch == '\\' || ch == '"' || ch == ';' ||
            ch == ':') {
            out.push_back('_');
        } else {
            out.push_back(ch);
        }
    }
    // Strip leading dots/spaces and collapse path traversal tokens.
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
        out.erase(out.begin());
    }
    while (out.find("..") != std::string::npos) {
        out.replace(out.find(".."), 2, "_");
    }
    if (out.empty()) {
        return "artifact.bin";
    }
    return out;
}

std::string build_content_disposition(std::string_view raw_display_name) {
    const std::string clean = sanitize_download_filename(raw_display_name);
    std::string ascii_fallback;
    ascii_fallback.reserve(clean.size());
    for (const char ch : clean) {
        const auto byte = static_cast<unsigned char>(ch);
        ascii_fallback.push_back((byte >= 0x20 && byte <= 0x7E) ? ch : '_');
    }
    return "attachment; filename=\"" + ascii_fallback + "\"; filename*=UTF-8''" +
           percent_encode_rfc5987(clean);
}

DownloadSession::DownloadSession(Options options)
: session_id_(options.session_id),
  principal_(std::move(options.principal)),
  chunk_bytes_(options.chunk_bytes),
  max_bytes_in_flight_(options.max_bytes_in_flight),
  range_(options.range),
  connection_(std::move(options.connection)),
  submit_disk_read_(std::move(options.submit_disk_read)),
  on_terminal_(std::move(options.on_terminal)),
  handle_(std::move(options.handle)),
  next_offset_(options.range.start),
  remaining_(options.range.length) {
    chunk_buffer_.resize(static_cast<std::size_t>(chunk_bytes_));
}

DownloadSession::~DownloadSession() noexcept = default;

std::shared_ptr<DownloadSession> DownloadSession::create(Options options) {
    return std::shared_ptr<DownloadSession>(new DownloadSession(std::move(options)));
}

void DownloadSession::attach_and_start(
    trantor::AsyncStreamPtr stream, std::string initial_header_bytes) {
    std::optional<TerminalInvocation> terminal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ || settled_) {
            return;
        }
        started_ = true;
        stream_ = std::move(stream);
        if (auto conn = connection_.lock()) {
            baseline_tcp_sent_ = conn->bytesSent();
            std::weak_ptr<DownloadSession> weak_self = shared_from_this();
            conn->setWriteCompleteCallback([weak_self](const trantor::TcpConnectionPtr&) {
                if (auto self = weak_self.lock()) {
                    self->on_transport_writable();
                }
            });
        }
        if (!stream_ || cancel_requested_) {
            eof_or_error_ = true;
            close_stream_locked();
            terminal = try_settle_locked();
        } else {
            bool header_ok = true;
            if (!initial_header_bytes.empty()) {
                header_ok = stream_->send(initial_header_bytes.data(), initial_header_bytes.size());
                if (header_ok) {
                    total_queued_bytes_ += initial_header_bytes.size();
                } else {
                    cancel_requested_ = true;
                    close_stream_locked();
                    terminal = try_settle_locked();
                }
            }
            if (header_ok) {
                if (remaining_ == 0) {
                    eof_or_error_ = true;
                    close_stream_locked();
                    terminal = try_settle_locked();
                } else {
                    terminal = pump_locked();
                }
            }
        }
    }
    invoke_terminal(std::move(terminal));
}

void DownloadSession::request_cancel() noexcept {
    std::shared_ptr<ArtifactReadHandle> handle_to_cancel;
    std::optional<TerminalInvocation> terminal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (settled_) {
            return;
        }
        cancel_requested_ = true;
        handle_to_cancel = handle_;
        if (!inflight_read_) {
            close_stream_locked();
            terminal = try_settle_locked();
        }
    }
    if (handle_to_cancel) {
        handle_to_cancel->request_cancel();
    }
    invoke_terminal(std::move(terminal));
}

void DownloadSession::on_transport_writable() noexcept {
    std::optional<TerminalInvocation> terminal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (settled_ || !started_) {
            return;
        }
        waiting_backpressure_ = false;
        terminal = pump_locked();
    }
    invoke_terminal(std::move(terminal));
}

bool DownloadSession::is_settled() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return settled_;
}

std::optional<DownloadSession::TerminalInvocation> DownloadSession::pump_locked() noexcept {
    if (settled_ || inflight_read_) {
        return std::nullopt;
    }
    if (cancel_requested_ || eof_or_error_ || remaining_ == 0 || !stream_) {
        close_stream_locked();
        return try_settle_locked();
    }
    auto conn = connection_.lock();
    if (!conn || conn->disconnected()) {
        cancel_requested_ = true;
        close_stream_locked();
        return try_settle_locked();
    }
    const std::uint64_t sent_now = conn->bytesSent();
    const std::uint64_t sent_delta =
        sent_now >= baseline_tcp_sent_ ? (sent_now - baseline_tcp_sent_) : 0;
    const std::uint64_t inflight_tcp =
        total_queued_bytes_ > sent_delta ? (total_queued_bytes_ - sent_delta) : 0;
    if (inflight_tcp >= max_bytes_in_flight_) {
        waiting_backpressure_ = true;
        // Safety poll on event loop in case write-complete fired just before check.
        if (auto* loop = conn->getLoop()) {
            try {
                std::weak_ptr<DownloadSession> weak_self = shared_from_this();
                loop->runAfter(0.02, [weak_self] {
                    if (auto self = weak_self.lock()) {
                        self->on_transport_writable();
                    }
                });
            } catch (...) {
                cancel_requested_ = true;
                close_stream_locked();
                return try_settle_locked();
            }
        }
        return std::nullopt;
    }
    return schedule_read_locked();
}

std::optional<DownloadSession::TerminalInvocation>
DownloadSession::schedule_read_locked() noexcept {
    const std::uint64_t offset = next_offset_;
    const std::size_t to_read =
        static_cast<std::size_t>(std::min<std::uint64_t>(chunk_bytes_, remaining_));
    inflight_read_ = true;
    bool admitted = false;
    if (submit_disk_read_) {
        try {
            admitted = submit_disk_read_(shared_from_this(), offset, to_read);
        } catch (...) {
            admitted = false;
        }
    }
    if (!admitted) {
        inflight_read_ = false;
        cancel_requested_ = true;
        close_stream_locked();
        return try_settle_locked();
    }
    return std::nullopt;
}

void DownloadSession::execute_disk_read(std::uint64_t offset, std::size_t to_read) {
    // Runs strictly on bounded download worker thread, never on Drogon event loop.
    std::shared_ptr<ArtifactReadHandle> handle;
    std::byte* buf_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handle = handle_;
        buf_ptr = chunk_buffer_.data();
    }
    Result<std::size_t> read_res =
        handle ? handle->read_at(offset, MutableBytes{buf_ptr, to_read})
               : Result<std::size_t>(Status(StatusCode::InvalidState, "handle missing"));

    std::optional<TerminalInvocation> terminal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inflight_read_ = false;
        if (cancel_requested_ || !stream_) {
            close_stream_locked();
            terminal = try_settle_locked();
        } else if (!read_res.ok() || read_res.value() == 0) {
            eof_or_error_ = true;
            close_stream_locked();
            terminal = try_settle_locked();
        } else {
            const std::size_t got = std::min<std::size_t>(read_res.value(), to_read);
            const bool sent_ok =
                stream_->send(reinterpret_cast<const char*>(chunk_buffer_.data()), got);
            if (!sent_ok) {
                cancel_requested_ = true;
                close_stream_locked();
                terminal = try_settle_locked();
            } else {
                next_offset_ += got;
                remaining_ -= got;
                total_queued_bytes_ += got;
                total_body_bytes_sent_ += got;
                if (remaining_ == 0) {
                    eof_or_error_ = true;
                    close_stream_locked();
                    terminal = try_settle_locked();
                } else {
                    terminal = pump_locked();
                }
            }
        }
    }
    invoke_terminal(std::move(terminal));
}

void DownloadSession::close_stream_locked() noexcept {
    auto conn = connection_.lock();
    if (conn) {
        conn->setWriteCompleteCallback(nullptr);
    }
    const bool truncated_abort = started_ && remaining_ > 0;
    const bool normal_finish = started_ && remaining_ == 0 && !cancel_requested_;
    if (stream_) {
        try {
            stream_->close();
        } catch (...) {
        }
        stream_.reset();
    }
    if (conn && !conn->disconnected()) {
        if (truncated_abort) {
            try {
                conn->forceClose();
            } catch (...) {
            }
        } else if (normal_finish) {
            try {
                if (auto* loop = conn->getLoop()) {
                    loop->queueInLoop([conn] {
                        if (!conn->disconnected()) {
                            conn->shutdown();
                        }
                    });
                } else {
                    conn->shutdown();
                }
            } catch (...) {
                try {
                    conn->forceClose();
                } catch (...) {
                }
            }
        }
    }
}

std::optional<DownloadSession::TerminalInvocation> DownloadSession::try_settle_locked() noexcept {
    if (settled_ || inflight_read_ || stream_) {
        return std::nullopt;
    }
    settled_ = true;
    // Release ArtifactReadHandle / ReadLease strictly once after in-flight read
    // has returned and transport stream has closed. Swap releases memory noexcept.
    handle_.reset();
    std::vector<std::byte>{}.swap(chunk_buffer_);
    if (!on_terminal_) {
        return std::nullopt;
    }
    TerminalInvocation inv;
    inv.callback = std::move(on_terminal_);
    inv.session_id = session_id_;
    inv.bytes_sent = total_body_bytes_sent_;
    inv.cancelled = cancel_requested_;
    return inv;
}

void DownloadSession::invoke_terminal(std::optional<TerminalInvocation> invocation) noexcept {
    if (!invocation || !invocation->callback) {
        return;
    }
    try {
        invocation->callback(invocation->session_id, invocation->bytes_sent, invocation->cancelled);
    } catch (...) {
    }
}

}  // namespace rodaq::server
