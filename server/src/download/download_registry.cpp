#include "download_registry.hpp"

#include <algorithm>
#include <utility>

namespace rodaq::server {

DownloadRegistry::DownloadRegistry(DownloadConfig config)
: config_(config), state_(std::make_shared<SharedState>()) {}

DownloadRegistry::~DownloadRegistry() noexcept {
    if (!stopped_) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

Result<std::unique_ptr<DownloadRegistry>> DownloadRegistry::create(const DownloadConfig& config) {
    try {
        std::unique_ptr<DownloadRegistry> reg(new DownloadRegistry(config));
        const std::size_t slot_capacity = std::max<std::size_t>(1, config.max_concurrent);
        reg->state_->cancel_scratch.resize(slot_capacity);
        reg->state_->read_ring.resize(slot_capacity);
        reg->state_->max_open_queue = std::max<std::size_t>(8, slot_capacity * 2);
        reg->workers_.reserve(config.workers);
        for (std::uint32_t i = 0; i < config.workers; ++i) {
            reg->workers_.emplace_back([state = reg->state_] { worker_loop(state); });
        }
        return reg;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "failed to allocate DownloadRegistry");
    }
}

bool DownloadRegistry::submit_open(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(state_->worker_mutex);
        if (state_->workers_stopping || state_->open_queue.size() >= state_->max_open_queue) {
            return false;
        }
        state_->open_queue.push_back(std::move(work));
    }
    state_->worker_cv.notify_one();
    return true;
}

DownloadAdmitResult DownloadRegistry::create_session(
    ByteRange range, std::shared_ptr<ArtifactReadHandle> handle,
    std::weak_ptr<trantor::TcpConnection> connection) {
    std::uint64_t sid = 0;
    const trantor::TcpConnection* raw_conn = nullptr;
    if (auto locked = connection.lock()) {
        raw_conn = locked.get();
    }
    {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        if (state_->stopping) {
            return {false, DownloadAdmissionFailure::ShuttingDown, {}};
        }
        if (state_->active_sessions.size() + state_->reserved_sessions >= config_.max_concurrent) {
            ++state_->metrics.rejected_global;
            return {false, DownloadAdmissionFailure::GlobalSaturation, {}};
        }
        sid = state_->next_session_id++;
        ++state_->reserved_sessions;
    }

    std::shared_ptr<DownloadSession> session;
    try {
        DownloadSession::Options opts;
        opts.session_id = sid;
        opts.chunk_bytes = config_.chunk_bytes;
        opts.max_bytes_in_flight = config_.max_bytes_in_flight;
        opts.range = range;
        opts.handle = std::move(handle);
        opts.connection = std::move(connection);
        opts.submit_disk_read = [state = state_](
                                    std::shared_ptr<DownloadSession> sess, std::uint64_t offset,
                                    std::size_t to_read) noexcept {
            return submit_disk_read(state, std::move(sess), offset, to_read);
        };
        opts.on_terminal = [state = state_](std::uint64_t id, std::uint64_t bytes, bool cancelled) {
            on_session_terminal(state, id, bytes, cancelled);
        };
        session = DownloadSession::create(std::move(opts));
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        --state_->reserved_sessions;
        if (state_->active_sessions.empty() && state_->reserved_sessions == 0) {
            state_->settled_cv.notify_all();
        }
        throw;
    }

    bool cancel_immediately = false;
    {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        --state_->reserved_sessions;
        state_->active_sessions.emplace(sid, session);
        if (raw_conn != nullptr) {
            state_->session_conns.emplace(sid, raw_conn);
            state_->conn_sessions[raw_conn].push_back(sid);
        }
        ++state_->metrics.total_started;
        state_->metrics.active = state_->active_sessions.size();
        state_->metrics.high_water_active =
            std::max(state_->metrics.high_water_active, state_->metrics.active);
        cancel_immediately = state_->stopping;
    }
    if (cancel_immediately) {
        session->request_cancel();
        return {false, DownloadAdmissionFailure::ShuttingDown, {}};
    }
    return {true, DownloadAdmissionFailure::GlobalSaturation, std::move(session)};
}

void DownloadRegistry::notify_connection_closed(const trantor::TcpConnection* raw_conn) noexcept {
    if (!raw_conn) {
        return;
    }
    std::lock_guard<std::mutex> cancel_lock(state_->cancel_mutex);
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        const auto it = state_->conn_sessions.find(raw_conn);
        if (it == state_->conn_sessions.end()) {
            return;
        }
        for (const auto sid : it->second) {
            const auto sit = state_->active_sessions.find(sid);
            if (sit != state_->active_sessions.end() && count < state_->cancel_scratch.size()) {
                state_->cancel_scratch[count++] = sit->second;
            }
            state_->session_conns.erase(sid);
        }
        state_->conn_sessions.erase(it);
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto session = std::move(state_->cancel_scratch[i]);
        if (session) {
            session->request_cancel();
        }
    }
}

void DownloadRegistry::cancel_all() noexcept {
    std::lock_guard<std::mutex> cancel_lock(state_->cancel_mutex);
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        state_->stopping = true;
        for (const auto& kv : state_->active_sessions) {
            if (count < state_->cancel_scratch.size()) {
                state_->cancel_scratch[count++] = kv.second;
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto session = std::move(state_->cancel_scratch[i]);
        if (session) {
            session->request_cancel();
        }
    }
}

Result<void> DownloadRegistry::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (stopped_) {
        return {};
    }
    cancel_all();
    {
        std::lock_guard<std::mutex> wlock(state_->worker_mutex);
        state_->open_queue.clear();
    }
    bool settled = false;
    {
        std::unique_lock<std::mutex> lock(state_->registry_mutex);
        settled = state_->settled_cv.wait_until(lock, deadline, [&] {
            return state_->active_sessions.empty() && state_->reserved_sessions == 0;
        });
    }
    {
        std::unique_lock<std::mutex> wlock(state_->worker_mutex);
        state_->workers_stopping = true;
        state_->worker_cv.notify_all();
        if (settled) {
            settled = state_->open_settled_cv.wait_until(
                wlock, deadline, [&] { return state_->active_opens == 0; });
        }
    }
    if (settled) {
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
        stopped_ = true;
        return {};
    }
    // Retain SharedState ownership on late-returning disk read rather than
    // destroying live ArtifactReadHandle backing or worker queue prematurely.
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.detach();
        }
    }
    stopped_ = true;
    return Status(StatusCode::DeadlineExceeded, "download read exceeded shutdown deadline");
}

std::uint64_t DownloadRegistry::active_count() const {
    std::lock_guard<std::mutex> lock(state_->registry_mutex);
    return state_->active_sessions.size();
}

DownloadMetrics DownloadRegistry::metrics() const {
    std::lock_guard<std::mutex> lock(state_->registry_mutex);
    return state_->metrics;
}

bool DownloadRegistry::submit_disk_read(
    const std::shared_ptr<SharedState>& state, std::shared_ptr<DownloadSession> session,
    std::uint64_t offset, std::size_t to_read) noexcept {
    {
        std::lock_guard<std::mutex> lock(state->worker_mutex);
        if (state->workers_stopping || state->read_ring.empty() ||
            state->ring_count >= state->read_ring.size()) {
            return false;
        }
        const std::size_t tail = (state->ring_head + state->ring_count) % state->read_ring.size();
        state->read_ring[tail] = {std::move(session), offset, to_read};
        ++state->ring_count;
    }
    state->worker_cv.notify_one();
    return true;
}

void DownloadRegistry::worker_loop(const std::shared_ptr<SharedState>& state) {
    for (;;) {
        ReadJob read_job;
        std::function<void()> open_job;
        {
            std::unique_lock<std::mutex> lock(state->worker_mutex);
            state->worker_cv.wait(lock, [&] {
                return state->workers_stopping || state->ring_count > 0 ||
                       !state->open_queue.empty();
            });
            if (state->ring_count > 0) {
                read_job = std::move(state->read_ring[state->ring_head]);
                state->read_ring[state->ring_head] = {};
                state->ring_head = (state->ring_head + 1) % state->read_ring.size();
                --state->ring_count;
            } else if (!state->open_queue.empty()) {
                open_job = std::move(state->open_queue.front());
                state->open_queue.pop_front();
                ++state->active_opens;
            } else if (state->workers_stopping) {
                break;
            } else {
                continue;
            }
        }
        if (read_job.session) {
            try {
                read_job.session->execute_disk_read(read_job.offset, read_job.to_read);
            } catch (...) {
                read_job.session->request_cancel();
            }
        } else if (open_job) {
            try {
                open_job();
            } catch (...) {
            }
            std::lock_guard<std::mutex> lock(state->worker_mutex);
            if (--state->active_opens == 0 && state->workers_stopping) {
                state->open_settled_cv.notify_all();
            }
        }
    }
}

void DownloadRegistry::on_session_terminal(
    const std::shared_ptr<SharedState>& state, std::uint64_t session_id, std::uint64_t bytes_sent,
    bool cancelled) {
    std::shared_ptr<DownloadSession> released;
    {
        std::lock_guard<std::mutex> lock(state->registry_mutex);
        const auto it = state->active_sessions.find(session_id);
        if (it != state->active_sessions.end()) {
            released = std::move(it->second);
            state->active_sessions.erase(it);
        }
        const auto sc_it = state->session_conns.find(session_id);
        if (sc_it != state->session_conns.end()) {
            const auto cs_it = state->conn_sessions.find(sc_it->second);
            if (cs_it != state->conn_sessions.end()) {
                auto& vec = cs_it->second;
                vec.erase(std::remove(vec.begin(), vec.end(), session_id), vec.end());
                if (vec.empty()) {
                    state->conn_sessions.erase(cs_it);
                }
            }
            state->session_conns.erase(sc_it);
        }
        state->metrics.active = state->active_sessions.size();
        state->metrics.total_bytes_sent += bytes_sent;
        if (cancelled) {
            ++state->metrics.total_cancelled;
        } else {
            ++state->metrics.total_completed;
        }
        if (state->active_sessions.empty() && state->reserved_sessions == 0) {
            state->settled_cv.notify_all();
        }
    }
}

}  // namespace rodaq::server
