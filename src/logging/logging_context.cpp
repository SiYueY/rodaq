#include "logging/logging_context.hpp"

#include "plugin/plugin_quarantine.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <utility>

namespace rodaq {
namespace {

thread_local const void* active_logging_state = nullptr;

bool is_log_level_valid(LogLevel level) noexcept {
    return level >= LogLevel::Trace && level <= LogLevel::Off;
}

std::size_t record_bytes(std::string_view category, std::string_view message) noexcept {
    if (category.size() > std::numeric_limits<std::size_t>::max() - message.size()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return category.size() + message.size();
}

}  // namespace

struct LoggingContext::State {
    explicit State(LoggingConfig value, std::shared_ptr<LoggingBackend> backend_value)
    : config(value), backend(std::move(backend_value)) {}

    LoggingConfig config;
    std::shared_ptr<LoggingBackend> backend;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<LogRecord> records;
    std::size_t queued_bytes = 0;
    std::uint64_t next_sequence = 1;
    std::uint64_t last_terminal_sequence = 0;
    LoggingStats stats;
    bool accepting = true;
    bool stopping = false;
    bool worker_done = false;
    bool backend_failed = false;
    std::shared_ptr<std::atomic<bool>> detached_done = std::make_shared<std::atomic<bool>>(false);
};

LoggingContext::LoggingContext(LoggingConfig config, std::shared_ptr<LoggingBackend> backend)
: config_(config), state_(std::make_shared<State>(config, std::move(backend))) {}

Result<std::unique_ptr<LoggingContext>> LoggingContext::create(
    LoggingConfig config, std::shared_ptr<LoggingBackend> backend) {
    if (config.queue_records == 0 || config.queue_bytes == 0 || config.flush_timeout.count() <= 0 ||
        config.shutdown_timeout.count() <= 0 || !is_log_level_valid(config.level) ||
        backend == nullptr) {
        return rodaq::Status(StatusCode::InvalidArgument, "logging configuration is invalid");
    }
    try {
        auto context =
            std::unique_ptr<LoggingContext>(new LoggingContext(config, std::move(backend)));
        context->worker_ = std::thread(
            [state = context->state_]() mutable { LoggingContext::worker_loop(std::move(state)); });
        return context;
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "logging initialization exhausted memory");
    } catch (const std::exception& error) {
        return rodaq::Status(
            StatusCode::Unavailable,
            "logging worker failed to start: " + std::string(error.what()));
    }
}

LoggingContext::~LoggingContext() {
    if (!worker_.joinable()) {
        return;
    }
    const bool stopped = shutdown(std::chrono::steady_clock::now() + config_.shutdown_timeout);
    if (!stopped) {
        quarantine_worker("Logging worker did not stop before destructor");
    }
}

void LoggingContext::log(
    LogLevel level, std::string_view category, std::string_view message) noexcept {
    try {
        if (!is_log_level_valid(level) || level < config_.level || level == LogLevel::Off) {
            return;
        }
        if (active_logging_state == state_.get()) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->stats.reentrant_drops;
            ++state_->stats.records_dropped;
            return;
        }
        if (category.empty() || !rodaq::is_valid_utf8_without_nul(category) ||
            !rodaq::is_valid_utf8_without_nul(message)) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->stats.records_dropped;
            state_->stats.bytes_dropped += record_bytes(category, message);
            return;
        }

        const std::size_t bytes = record_bytes(category, message);
        LogRecord record;
        record.level = level;
        record.category.assign(category.data(), category.size());
        record.message.assign(message.data(), message.size());

        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->stats.records_submitted;
        if (!state_->accepting || state_->backend_failed ||
            state_->records.size() >= config_.queue_records ||
            bytes > config_.queue_bytes - state_->queued_bytes) {
            ++state_->stats.records_dropped;
            state_->stats.bytes_dropped += bytes;
            return;
        }
        record.sequence = state_->next_sequence++;
        state_->records.push_back(std::move(record));
        state_->queued_bytes += bytes;
        state_->stats.queue_records_current = state_->records.size();
        state_->stats.queue_bytes_current = state_->queued_bytes;
        state_->stats.queue_records_high_water =
            std::max<std::uint64_t>(state_->stats.queue_records_high_water, state_->records.size());
        state_->stats.queue_bytes_high_water =
            std::max<std::uint64_t>(state_->stats.queue_bytes_high_water, state_->queued_bytes);
        state_->condition.notify_one();
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->stats.records_dropped;
            state_->stats.bytes_dropped += record_bytes(category, message);
        } catch (...) {
        }
    }
}

void LoggingContext::worker_loop(std::shared_ptr<State> state) noexcept {
    struct LoggingReentryGuard {
        LoggingReentryGuard(const void* logging_state, const void* previous_state) noexcept
        : previous(previous_state) {
            active_logging_state = logging_state;
        }
        ~LoggingReentryGuard() { active_logging_state = previous; }

        const void* previous;
    };

    while (true) {
        LogRecord record;
        bool backend_failed = false;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->condition.wait(
                lock, [&state] { return state->stopping || !state->records.empty(); });
            if (state->records.empty()) {
                state->worker_done = true;
                state->detached_done->store(true, std::memory_order_release);
                state->condition.notify_all();
                return;
            }
            record = std::move(state->records.front());
            state->records.pop_front();
            state->queued_bytes -= record.category.size() + record.message.size();
            state->stats.queue_records_current = state->records.size();
            state->stats.queue_bytes_current = state->queued_bytes;
            backend_failed = state->backend_failed;
        }

        bool written = false;
        if (!backend_failed) {
            LoggingReentryGuard guard(state.get(), active_logging_state);
            try {
                written = state->backend->write(record).ok();
            } catch (...) {
                written = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->last_terminal_sequence = record.sequence;
            if (written) {
                ++state->stats.records_written;
            } else {
                ++state->stats.records_dropped;
                state->stats.bytes_dropped += record.category.size() + record.message.size();
                ++state->stats.backend_failures;
                state->backend_failed = true;
            }
            state->condition.notify_all();
        }
    }
}

bool LoggingContext::flush(std::chrono::steady_clock::time_point deadline) noexcept {
    std::uint64_t target = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        target = state_->next_sequence == 0 ? 0 : state_->next_sequence - 1U;
    }
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (!state_->condition.wait_until(lock, deadline, [this, target] {
                return state_->last_terminal_sequence >= target;
            })) {
            state_->stats.flush_timeouts++;
            return false;
        }
    }
    try {
        auto flushed = state_->backend->flush();
        if (!flushed.ok()) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->stats.backend_failures;
            state_->backend_failed = true;
            return false;
        }
        return true;
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->stats.backend_failures;
        state_->backend_failed = true;
        return false;
    }
}

bool LoggingContext::shutdown(std::chrono::steady_clock::time_point deadline) noexcept {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting = false;
        state_->stopping = true;
        state_->condition.notify_all();
    }
    if (worker_.joinable()) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (!state_->condition.wait_until(lock, deadline, [this] { return state_->worker_done; })) {
            state_->stats.shutdown_timeouts++;
            return false;
        }
        lock.unlock();
        worker_.join();
    }
    try {
        auto shutdown = state_->backend->shutdown();
        if (!shutdown.ok()) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->stats.backend_failures;
            return false;
        }
        return true;
    } catch (...) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->stats.backend_failures;
        return false;
    }
}

void LoggingContext::quarantine_worker(std::string reason) noexcept {
    if (!worker_.joinable()) {
        return;
    }
    // The worker updates State::detached_done; retain the full state with the
    // thread rather than detaching it.
    auto state = state_;
    try {
        if (!plugin::PluginQuarantine::process().retain_worker(
                std::move(worker_), state, state_->detached_done, std::move(reason))) {
            std::abort();
        }
    } catch (...) {
        std::abort();
    }
}

LoggingStats LoggingContext::stats() const noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
}

}  // namespace rodaq
