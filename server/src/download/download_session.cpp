#include "download_session.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <utility>

namespace rodaq::server {
namespace {

bool parse_u64_part(std::string_view s, std::uint64_t& out) noexcept {
    if (s.empty() || s.size() > 20) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

}  // namespace

RangeParseResult parse_single_byte_range(
    std::string_view header, std::uint64_t full_length) noexcept {
    if (header.empty()) {
        return {RangeParseOutcome::None, {}};
    }
    constexpr std::string_view kPrefix = "bytes=";
    if (header.size() <= kPrefix.size() || header.substr(0, kPrefix.size()) != kPrefix) {
        return {RangeParseOutcome::Malformed, {}};
    }
    const auto spec = header.substr(kPrefix.size());
    if (spec.find(',') != std::string_view::npos) {
        return {RangeParseOutcome::MultipleNotSupported, {}};
    }
    const auto dash = spec.find('-');
    if (dash == std::string_view::npos) {
        return {RangeParseOutcome::Malformed, {}};
    }
    const auto lhs = spec.substr(0, dash);
    const auto rhs = spec.substr(dash + 1);
    if (lhs.empty() && rhs.empty()) {
        return {RangeParseOutcome::Malformed, {}};
    }

    if (lhs.empty()) {
        // Suffix range: bytes=-suffix_length
        std::uint64_t suffix = 0;
        if (!parse_u64_part(rhs, suffix)) {
            return {RangeParseOutcome::Malformed, {}};
        }
        if (suffix == 0 || full_length == 0) {
            return {RangeParseOutcome::Unsatisfiable, {}};
        }
        const auto len = std::min(suffix, full_length);
        return {RangeParseOutcome::Valid, ByteRange{full_length - len, full_length - 1}};
    }

    std::uint64_t start = 0;
    if (!parse_u64_part(lhs, start)) {
        return {RangeParseOutcome::Malformed, {}};
    }
    if (rhs.empty()) {
        // Open-ended range: bytes=start-
        if (full_length == 0 || start >= full_length) {
            return {RangeParseOutcome::Unsatisfiable, {}};
        }
        return {RangeParseOutcome::Valid, ByteRange{start, full_length - 1}};
    }

    std::uint64_t end = 0;
    if (!parse_u64_part(rhs, end)) {
        return {RangeParseOutcome::Malformed, {}};
    }
    if (start > end) {
        return {RangeParseOutcome::Malformed, {}};
    }
    if (full_length == 0 || start >= full_length) {
        return {RangeParseOutcome::Unsatisfiable, {}};
    }
    return {RangeParseOutcome::Valid, ByteRange{start, std::min(end, full_length - 1)}};
}

std::string sanitize_display_filename(std::string_view raw) {
    std::string out;
    out.reserve(std::min<std::size_t>(raw.size(), 128));
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == '"' || c == ';') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
        if (out.size() >= 128) {
            break;
        }
    }
    if (out.empty() || out == "." || out == "..") {
        return "artifact.bin";
    }
    return out;
}

DownloadSession::DownloadSession(
    DownloadRegistry* registry, ArtifactReadHandle handle, ByteRange range,
    std::uint64_t chunk_bytes, drogon::ResponseStreamPtr stream)
: registry_(registry),
  handle_(std::move(handle)),
  range_(range),
  chunk_bytes_(chunk_bytes),
  stream_(std::move(stream)) {}

void DownloadSession::request_cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_release);
    if (handle_) {
        handle_->request_cancel();
    }
}

void DownloadSession::run_on_worker() noexcept {
    try {
        std::string buffer;
        buffer.resize(static_cast<std::size_t>(std::min(chunk_bytes_, range_.length())));
        std::uint64_t offset = range_.start;
        std::uint64_t remaining = range_.length();

        while (remaining > 0 && !cancel_requested_.load(std::memory_order_acquire)) {
            const auto to_read =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
            auto read_res = handle_->read_at(
                offset, MutableBytes{reinterpret_cast<std::byte*>(buffer.data()), to_read});
            if (!read_res.ok() || read_res.value() == 0) {
                break;
            }
            if (cancel_requested_.load(std::memory_order_acquire)) {
                break;
            }
            const auto got = read_res.value();
            if (!stream_->send(buffer.substr(0, got))) {
                cancel_requested_.store(true, std::memory_order_release);
                break;
            }
            offset += got;
            remaining -= got;
        }
    } catch (...) {
        cancel_requested_.store(true, std::memory_order_release);
    }

    if (stream_) {
        stream_->close();
        stream_.reset();
    }
    // Exactly-once release of ArtifactReadHandle / ReadLease after in-flight read has settled.
    handle_.reset();
    if (registry_) {
        registry_->finish_session(shared_from_this());
    }
}

DownloadRegistry::DownloadRegistry(DownloadConfig config) : config_(config) {
    workers_.reserve(config_.workers);
    for (std::uint32_t i = 0; i < config_.workers; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

Result<std::unique_ptr<DownloadRegistry>> DownloadRegistry::create(const DownloadConfig& config) {
    try {
        return std::unique_ptr<DownloadRegistry>(new DownloadRegistry(config));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "Failed to allocate DownloadRegistry");
    }
}

DownloadRegistry::~DownloadRegistry() noexcept {
    if (!stopped_) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

Result<std::shared_ptr<DownloadSession>> DownloadRegistry::start_session(
    ArtifactReadHandle handle, ByteRange range, drogon::ResponseStreamPtr stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || active_.size() >= config_.max_concurrent) {
        return Status(StatusCode::Unavailable, "Download capacity exhausted");
    }
    auto session = std::make_shared<DownloadSession>(
        this, std::move(handle), range, config_.chunk_bytes, std::move(stream));
    active_.insert(session);
    queue_.push_back(session);
    cv_.notify_one();
    return session;
}

void DownloadRegistry::cancel_all() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& session : active_) {
        session->request_cancel();
    }
}

void DownloadRegistry::finish_session(const std::shared_ptr<DownloadSession>& session) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    active_.erase(session);
    if (active_.empty()) {
        empty_cv_.notify_all();
    }
}

void DownloadRegistry::worker_loop() noexcept {
    for (;;) {
        std::shared_ptr<DownloadSession> session;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                return;
            }
            session = std::move(queue_.front());
            queue_.pop_front();
        }
        session->run_on_worker();
    }
}

Result<void> DownloadRegistry::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (stopped_) {
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (const auto& session : active_) {
            session->request_cancel();
        }
    }
    cv_.notify_all();

    bool drained = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        drained = empty_cv_.wait_until(lock, deadline, [this]() { return active_.empty(); });
    }
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    stopped_ = true;
    return drained ? Result<void>{}
                   : Result<void>(Status(StatusCode::DeadlineExceeded, "Download drain timeout"));
}

std::size_t DownloadRegistry::active_downloads() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.size();
}

}  // namespace rodaq::server
