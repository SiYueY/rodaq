#include "download_registry.hpp"

#include <algorithm>
#include <utility>

namespace rodaq::server {

DownloadRegistry::DownloadRegistry(DownloadConfig config)
: config_(config),
  max_per_principal_(std::max<std::size_t>(2, config.max_concurrent / 2)),
  state_(std::make_shared<SharedState>()) {}

DownloadRegistry::~DownloadRegistry() noexcept {
    if (!stopped_) {
        (void)shutdown(std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
}

Result<std::unique_ptr<DownloadRegistry>> DownloadRegistry::create(const DownloadConfig& config) {
    try {
        std::unique_ptr<DownloadRegistry> reg(new DownloadRegistry(config));
        reg->workers_.reserve(config.workers);
        for (std::uint32_t i = 0; i < config.workers; ++i) {
            reg->workers_.emplace_back([state = reg->state_] { worker_loop(state); });
        }
        return reg;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "failed to allocate DownloadRegistry");
    }
}

DownloadAdmitResult DownloadRegistry::create_session(
    const std::string& principal, ByteRange range, std::shared_ptr<ArtifactReadHandle> handle,
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
        if (!principal.empty()) {
            const auto it = state_->principal_sessions.find(principal);
            if (it != state_->principal_sessions.end() && it->second >= max_per_principal_) {
                ++state_->metrics.rejected_principal;
                return {false, DownloadAdmissionFailure::PrincipalQuota, {}};
            }
        }
        sid = state_->next_session_id++;
        ++state_->reserved_sessions;
        if (!principal.empty()) {
            ++state_->principal_sessions[principal];
        }
    }

    std::shared_ptr<DownloadSession> session;
    try {
        DownloadSession::Options opts;
        opts.session_id = sid;
        opts.principal = principal;
        opts.chunk_bytes = config_.chunk_bytes;
        opts.max_bytes_in_flight = config_.max_bytes_in_flight;
        opts.range = range;
        opts.handle = std::move(handle);
        opts.connection = std::move(connection);
        opts.submit_disk_read = [state = state_](std::function<void()> fn) {
            submit_disk_read(state, std::move(fn));
        };
        opts.on_terminal = [state = state_](std::uint64_t id, std::uint64_t bytes, bool cancelled) {
            on_session_terminal(state, id, bytes, cancelled);
        };
        session = DownloadSession::create(std::move(opts));
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        --state_->reserved_sessions;
        if (!principal.empty()) {
            const auto pit = state_->principal_sessions.find(principal);
            if (pit != state_->principal_sessions.end() && --pit->second == 0) {
                state_->principal_sessions.erase(pit);
            }
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
    std::vector<std::shared_ptr<DownloadSession>> to_cancel;
    {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        const auto it = state_->conn_sessions.find(raw_conn);
        if (it == state_->conn_sessions.end()) {
            return;
        }
        for (const auto sid : it->second) {
            const auto sit = state_->active_sessions.find(sid);
            if (sit != state_->active_sessions.end()) {
                to_cancel.push_back(sit->second);
            }
            state_->session_conns.erase(sid);
        }
        state_->conn_sessions.erase(it);
    }
    for (const auto& session : to_cancel) {
        session->request_cancel();
    }
}

void DownloadRegistry::cancel_all() noexcept {
    std::vector<std::shared_ptr<DownloadSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(state_->registry_mutex);
        state_->stopping = true;
        sessions.reserve(state_->active_sessions.size());
        for (const auto& kv : state_->active_sessions) {
            sessions.push_back(kv.second);
        }
    }
    for (const auto& session : sessions) {
        session->request_cancel();
    }
}

Result<void> DownloadRegistry::shutdown(std::chrono::steady_clock::time_point deadline) {
    if (stopped_) {
        return {};
    }
    cancel_all();
    bool settled = false;
    {
        std::unique_lock<std::mutex> lock(state_->registry_mutex);
        settled = state_->settled_cv.wait_until(lock, deadline, [&] {
            return state_->active_sessions.empty() && state_->reserved_sessions == 0;
        });
    }
    {
        std::lock_guard<std::mutex> wlock(state_->worker_mutex);
        state_->workers_stopping = true;
    }
    state_->worker_cv.notify_all();
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

void DownloadRegistry::submit_disk_read(
    const std::shared_ptr<SharedState>& state, std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(state->worker_mutex);
        state->read_queue.push_back(std::move(task));
    }
    state->worker_cv.notify_one();
}

void DownloadRegistry::worker_loop(const std::shared_ptr<SharedState>& state) {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(state->worker_mutex);
            state->worker_cv.wait(
                lock, [&] { return state->workers_stopping || !state->read_queue.empty(); });
            if (state->read_queue.empty()) {
                if (state->workers_stopping) {
                    break;
                }
                continue;
            }
            task = std::move(state->read_queue.front());
            state->read_queue.pop_front();
        }
        try {
            if (task) {
                task();
            }
        } catch (...) {
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
            const std::string& principal = released->principal();
            if (!principal.empty()) {
                const auto pit = state->principal_sessions.find(principal);
                if (pit != state->principal_sessions.end() && --pit->second == 0) {
                    state->principal_sessions.erase(pit);
                }
            }
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
