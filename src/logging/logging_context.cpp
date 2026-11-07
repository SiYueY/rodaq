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

thread_local const LoggerState* active_logging_state = nullptr;

constexpr std::size_t kMaxAsyncQueueRecords = 262144;
constexpr std::size_t kMaxBacktraceCapacity = 65536;

bool is_valid_level(LogLevel level) noexcept {
    return level >= LogLevel::Trace && level <= LogLevel::Off;
}

bool is_valid_time_zone(TimeZone time_zone) noexcept {
    return time_zone == TimeZone::Local || time_zone == TimeZone::Utc;
}

bool is_valid_overflow_policy(OverflowPolicy policy) noexcept {
    return policy == OverflowPolicy::Block || policy == OverflowPolicy::OverrunOldest ||
           policy == OverflowPolicy::DiscardNew;
}

bool is_valid_console_stream(ConsoleStream stream) noexcept {
    return stream == ConsoleStream::Stdout || stream == ConsoleStream::Stderr;
}

std::size_t add_bytes(std::size_t current, std::size_t additional) noexcept {
    if (current > std::numeric_limits<std::size_t>::max() - additional) {
        return std::numeric_limits<std::size_t>::max();
    }
    return current + additional;
}

std::size_t record_bytes(std::string_view category, std::string_view message) noexcept {
    return add_bytes(category.size(), message.size());
}

std::size_t record_bytes(const LogRecord& record) noexcept {
    return add_bytes(
        add_bytes(record.category.size(), record.message.size()),
        add_bytes(record.source_file.size(), record.source_function.size()));
}

Result<void> validate_sink_options(const SinkOptions& options) {
    if (!is_valid_level(options.level)) {
        return Status(StatusCode::InvalidArgument, "config", 0, "logging sink level is invalid");
    }
    return {};
}

Result<void> validate_sink(const ConsoleSinkConfig& sink) {
    if (!is_valid_console_stream(sink.stream)) {
        return Status(
            StatusCode::InvalidArgument, "config", 0, "logging console stream is invalid");
    }
    return validate_sink_options(sink.options);
}

Result<void> validate_sink(const FileSinkConfig& sink) {
    if (sink.path.empty()) {
        return Status(
            StatusCode::InvalidArgument, "config", 0, "logging file path must not be empty");
    }
    return validate_sink_options(sink.options);
}

Result<void> validate_sink(const RotatingFileSinkConfig& sink) {
    if (sink.path.empty() || sink.max_size == 0 || sink.max_files == 0) {
        return Status(
            StatusCode::InvalidArgument, "config", 0,
            "rotating logging path and capacities must be configured");
    }
    return validate_sink_options(sink.options);
}

Result<void> validate_sink(const DailyFileSinkConfig& sink) {
    if (sink.path.empty() || sink.rotation_hour > 23 || sink.rotation_minute > 59) {
        return Status(
            StatusCode::InvalidArgument, "config", 0,
            "daily logging path or rotation time is invalid");
    }
    return validate_sink_options(sink.options);
}

Result<void> validate_config(const LoggingConfig& config) {
    if (config.name.empty()) {
        return Status(StatusCode::InvalidArgument, "config", 0, "logging name must not be empty");
    }
    if (!is_valid_level(config.level) || !is_valid_level(config.flush.level)) {
        return Status(StatusCode::InvalidArgument, "config", 0, "logging level is invalid");
    }
    if (!is_valid_time_zone(config.time_zone)) {
        return Status(StatusCode::InvalidArgument, "config", 0, "logging time zone is invalid");
    }
    if (!is_valid_overflow_policy(config.async.overflow_policy)) {
        return Status(
            StatusCode::InvalidArgument, "config", 0, "logging overflow policy is invalid");
    }
    if (config.pattern.empty() || config.sinks.empty()) {
        return Status(
            StatusCode::InvalidArgument, "config", 0,
            "logging pattern and sinks must not be empty");
    }
    if (config.async.enabled &&
        (config.async.queue_records == 0 || config.async.queue_records > kMaxAsyncQueueRecords ||
         config.async.queue_bytes == 0)) {
        return Status(
            StatusCode::InvalidArgument, "config", 0, "async logging queue capacity is invalid");
    }
    if (config.flush.interval.count() < 0 || config.backtrace_capacity > kMaxBacktraceCapacity ||
        config.flush_timeout.count() <= 0 || config.shutdown_timeout.count() <= 0) {
        return Status(
            StatusCode::InvalidArgument, "config", 0,
            "logging timeout or backtrace capacity is invalid");
    }
    for (const SinkConfig& sink : config.sinks) {
        const auto result =
            std::visit([](const auto& value) { return validate_sink(value); }, sink);
        if (!result.ok()) {
            return result;
        }
    }
    return {};
}

bool can_fit_locked(
    const std::deque<LoggerState::QueueItem>& queue, std::size_t queued_bytes, std::size_t bytes,
    const LoggingConfig& config) noexcept {
    return queue.size() < config.async.queue_records &&
           bytes <= config.async.queue_bytes - queued_bytes;
}

}  // namespace

LoggerState::LoggerState(LoggingConfig value, std::shared_ptr<LoggingBackend> logging_backend)
: config(std::move(value)), backend(std::move(logging_backend)), level(config.level) {
    stats_value.initialized = true;
    stats_value.asynchronous = config.async.enabled;
    stats_value.queue_records_capacity = config.async.queue_records;
    stats_value.queue_bytes_capacity = config.async.queue_bytes;
}

LoggingStats LoggerState::stats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    LoggingStats snapshot = stats_value;
    snapshot.backtrace_enabled = backtrace_enabled.load(std::memory_order_acquire);
    return snapshot;
}

void LoggerState::record_backend_failure() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex);
        ++stats_value.backend_failures;
    } catch (...) {
    }
}

LoggingContext::LoggingContext(LoggingConfig config, std::shared_ptr<LoggingBackend> backend)
: config_(std::move(config)), state_(std::make_shared<LoggerState>(config_, std::move(backend))) {}

Result<std::unique_ptr<LoggingContext>> LoggingContext::create(
    LoggingConfig config, std::shared_ptr<LoggingBackend> backend) {
    auto valid = validate_config(config);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    if (backend == nullptr) {
        return Status(StatusCode::InvalidArgument, "config", 0, "logging backend is null");
    }
    try {
        auto context =
            std::unique_ptr<LoggingContext>(new LoggingContext(std::move(config), backend));
        if (context->config_.backtrace_capacity != 0) {
            auto backtrace = backend->enable_backtrace(context->config_.backtrace_capacity);
            if (!backtrace.ok()) {
                return std::move(backtrace).status();
            }
            context->state_->backtrace_enabled.store(true, std::memory_order_release);
        }
        context->worker_ = std::thread(
            [state = context->state_]() mutable { LoggingContext::worker_loop(std::move(state)); });
        return context;
    } catch (const std::bad_alloc&) {
        return Status(
            StatusCode::ResourceExhausted, "logging", 0, "logging initialization exhausted memory");
    } catch (const std::exception& error) {
        return Status(
            StatusCode::Unavailable, "logging", 0,
            std::string("logging worker failed to start: ") + error.what());
    }
}

LoggingContext::~LoggingContext() {
    if (!worker_.joinable()) {
        return;
    }
    if (!shutdown(std::chrono::steady_clock::now() + config_.shutdown_timeout)) {
        quarantine_worker("Logging worker did not stop before destructor");
    }
}

Result<Logger> LoggingContext::make_logger(std::string_view category) {
    if (category.empty() || !is_valid_utf8_without_nul(category)) {
        return Status(
            StatusCode::InvalidArgument, "logging", 0,
            "logging category must be non-empty UTF-8 without NUL");
    }
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is stopping");
    }
    return Logger(state_, std::string(category));
}

void LoggingContext::log(
    LogLevel level_value, std::string_view category, SourceLocation source,
    std::string_view message) noexcept {
    auto logger = make_logger(category);
    if (logger.ok()) {
        logger.value().write(level_value, source, message);
    }
}

void LoggingContext::worker_loop(std::shared_ptr<LoggerState> state) noexcept {
    struct ReentryGuard {
        ReentryGuard(const LoggerState* state_value, const LoggerState* previous_value) noexcept
        : previous(previous_value) {
            active_logging_state = state_value;
        }
        ~ReentryGuard() { active_logging_state = previous; }

        const LoggerState* previous;
    };

    while (true) {
        LoggerState::QueueItem item;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->queue.empty() && !state->stopping &&
                state->config.flush.interval.count() > 0) {
                const bool wake = state->condition.wait_for(
                    lock, state->config.flush.interval,
                    [&state] { return state->stopping || !state->queue.empty(); });
                if (!wake) {
                    lock.unlock();
                    ReentryGuard guard(state.get(), active_logging_state);
                    try {
                        if (!state->backend->flush().ok()) {
                            lock.lock();
                            ++state->stats_value.backend_failures;
                            lock.unlock();
                        }
                    } catch (...) {
                        lock.lock();
                        ++state->stats_value.backend_failures;
                        lock.unlock();
                    }
                    continue;
                }
            } else {
                state->condition.wait(
                    lock, [&state] { return state->stopping || !state->queue.empty(); });
            }
            if (state->queue.empty() && state->stopping) {
                state->worker_done = true;
                state->worker_done_signal->store(true, std::memory_order_release);
                state->condition.notify_all();
                return;
            }
            item = std::move(state->queue.front());
            state->queue.pop_front();
            if (auto* log_item = std::get_if<LoggerState::LogItem>(&item)) {
                state->queued_bytes -= record_bytes(log_item->record);
                state->stats_value.queue_records_current = state->queue.size();
                state->stats_value.queue_bytes_current = state->queued_bytes;
            }
        }

        ReentryGuard guard(state.get(), active_logging_state);
        if (auto* log_item = std::get_if<LoggerState::LogItem>(&item)) {
            bool written = false;
            try {
                written = state->backend->write(log_item->record).ok();
            } catch (...) {
                written = false;
            }
            bool flush_after = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->last_terminal_sequence = log_item->record.sequence;
                if (written) {
                    ++state->stats_value.records_written;
                    flush_after = log_item->record.level >= state->config.flush.level;
                } else {
                    ++state->stats_value.records_dropped;
                    state->stats_value.bytes_dropped += record_bytes(log_item->record);
                    ++state->stats_value.backend_failures;
                    state->backend_failed = true;
                }
                state->condition.notify_all();
            }
            if (flush_after) {
                try {
                    if (!state->backend->flush().ok()) {
                        state->record_backend_failure();
                    }
                } catch (...) {
                    state->record_backend_failure();
                }
            }
        } else if (std::holds_alternative<LoggerState::FlushItem>(item)) {
            try {
                if (!state->backend->flush().ok()) {
                    state->record_backend_failure();
                }
            } catch (...) {
                state->record_backend_failure();
            }
        } else {
            try {
                if (!state->backend->dump_backtrace().ok()) {
                    state->record_backend_failure();
                }
            } catch (...) {
                state->record_backend_failure();
            }
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
                return state_->active_writers == 0 && state_->queue.empty() &&
                       state_->last_terminal_sequence >= target;
            })) {
            ++state_->stats_value.flush_timeouts;
            return false;
        }
    }
    try {
        const bool flushed = state_->backend->flush().ok();
        if (!flushed) {
            state_->record_backend_failure();
        }
        return flushed;
    } catch (...) {
        state_->record_backend_failure();
        return false;
    }
}

bool LoggingContext::shutdown(std::chrono::steady_clock::time_point deadline) noexcept {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting.store(false, std::memory_order_release);
        state_->stopping = true;
        state_->condition.notify_all();
    }
    if (worker_.joinable()) {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (!state_->condition.wait_until(lock, deadline, [this] {
                return state_->worker_done && state_->active_writers == 0;
            })) {
            ++state_->stats_value.shutdown_timeouts;
            return false;
        }
        lock.unlock();
        worker_.join();
    }
    try {
        const bool stopped = state_->backend->shutdown().ok();
        if (!stopped) {
            state_->record_backend_failure();
        }
        return stopped;
    } catch (...) {
        state_->record_backend_failure();
        return false;
    }
}

void LoggingContext::quarantine_worker(std::string reason) noexcept {
    if (!worker_.joinable()) {
        return;
    }
    auto state = state_;
    try {
        if (!plugin::PluginQuarantine::process().retain_worker(
                std::move(worker_), state, state->worker_done_signal, std::move(reason))) {
            std::abort();
        }
    } catch (...) {
        std::abort();
    }
}

LoggingStats LoggingContext::stats() const noexcept { return state_->stats(); }

Logger::Logger(std::weak_ptr<LoggerState> state, std::string category)
: state_(std::move(state)), category_(std::move(category)) {}

bool Logger::valid() const noexcept {
    auto state = state_.lock();
    return state != nullptr && state->accepting.load(std::memory_order_acquire);
}

LogLevel Logger::level() const noexcept {
    auto state = state_.lock();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return LogLevel::Off;
    }
    return state->level.load(std::memory_order_acquire);
}

Result<void> Logger::set_level(LogLevel level_value) {
    if (!is_valid_level(level_value)) {
        return Status(StatusCode::InvalidArgument, "logging", 0, "logging level is invalid");
    }
    auto state = state_.lock();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is not initialized");
    }
    try {
        auto result = state->backend->set_level(level_value);
        if (!result.ok()) {
            state->record_backend_failure();
            return result;
        }
    } catch (const std::exception& error) {
        state->record_backend_failure();
        return Status(
            StatusCode::Unavailable, "logging", 0,
            std::string("failed to set logging level: ") + error.what());
    } catch (...) {
        state->record_backend_failure();
        return Status(StatusCode::Unavailable, "logging", 0, "unknown logging level failure");
    }
    state->level.store(level_value, std::memory_order_release);
    return {};
}

bool Logger::should_log(LogLevel level_value) noexcept {
    if (!is_valid_level(level_value) || level_value == LogLevel::Off) {
        return false;
    }
    auto state = state_.lock();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return false;
    }
    if (state->backtrace_enabled.load(std::memory_order_acquire)) {
        return true;
    }
    return level_value >= state->level.load(std::memory_order_acquire);
}

void Logger::write(LogLevel level_value, SourceLocation source, std::string_view message) noexcept {
    if (!is_valid_level(level_value) || level_value == LogLevel::Off) {
        return;
    }
    auto state = state_.lock();
    if (state == nullptr) {
        return;
    }
    if (!state->backtrace_enabled.load(std::memory_order_acquire) &&
        level_value < state->level.load(std::memory_order_acquire)) {
        return;
    }
    if (active_logging_state == state.get()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->stats_value.reentrant_drops;
        ++state->stats_value.records_dropped;
        return;
    }
    struct ActiveWriter {
        std::shared_ptr<LoggerState> state;
        bool active = false;

        ~ActiveWriter() {
            if (!active) {
                return;
            }
            try {
                std::lock_guard<std::mutex> lock(state->mutex);
                release_locked();
            } catch (...) {
            }
        }

        void activate_locked() noexcept {
            ++state->active_writers;
            active = true;
        }

        void release_locked() noexcept {
            if (!active) {
                return;
            }
            --state->active_writers;
            active = false;
            state->condition.notify_all();
        }
    };
    ActiveWriter writer{state};
    try {
        if (category_.empty() || !is_valid_utf8_without_nul(category_) ||
            !is_valid_utf8_without_nul(message)) {
            std::lock_guard<std::mutex> lock(state->mutex);
            ++state->stats_value.records_dropped;
            state->stats_value.bytes_dropped += record_bytes(category_, message);
            return;
        }

        LogRecord record;
        record.level = level_value;
        record.category = category_;
        record.message.assign(message.data(), message.size());
        if (source.file != nullptr) {
            record.source_file.assign(source.file);
        }
        if (source.function != nullptr) {
            record.source_function.assign(source.function);
        }
        record.source_line = source.line;
        const std::size_t bytes = record_bytes(record);

        if (!state->config.async.enabled) {
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                if (!state->accepting.load(std::memory_order_acquire) || state->backend_failed) {
                    ++state->stats_value.records_dropped;
                    state->stats_value.bytes_dropped += bytes;
                    return;
                }
                writer.activate_locked();
                ++state->stats_value.records_submitted;
                record.sequence = state->next_sequence++;
            }
            bool written = false;
            try {
                written = state->backend->write(record).ok();
            } catch (...) {
            }
            bool flush_after = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->last_terminal_sequence = record.sequence;
                if (written) {
                    ++state->stats_value.records_written;
                    flush_after = record.level >= state->config.flush.level;
                } else {
                    ++state->stats_value.records_dropped;
                    state->stats_value.bytes_dropped += bytes;
                    ++state->stats_value.backend_failures;
                    state->backend_failed = true;
                }
                writer.release_locked();
                state->condition.notify_all();
            }
            if (flush_after) {
                try {
                    if (!state->backend->flush().ok()) {
                        state->record_backend_failure();
                    }
                } catch (...) {
                    state->record_backend_failure();
                }
            }
            return;
        }

        std::unique_lock<std::mutex> lock(state->mutex);
        writer.activate_locked();
        ++state->stats_value.records_submitted;
        if (!state->accepting.load(std::memory_order_acquire) || state->backend_failed) {
            ++state->stats_value.records_dropped;
            state->stats_value.bytes_dropped += bytes;
            writer.release_locked();
            return;
        }
        if (!can_fit_locked(state->queue, state->queued_bytes, bytes, state->config)) {
            if (bytes > state->config.async.queue_bytes) {
                ++state->stats_value.records_dropped;
                ++state->stats_value.discard_count;
                state->stats_value.bytes_dropped += bytes;
                writer.release_locked();
                return;
            }
            if (state->config.async.overflow_policy == OverflowPolicy::DiscardNew) {
                ++state->stats_value.records_dropped;
                ++state->stats_value.discard_count;
                state->stats_value.bytes_dropped += bytes;
                writer.release_locked();
                return;
            }
            if (state->config.async.overflow_policy == OverflowPolicy::OverrunOldest) {
                while (!can_fit_locked(state->queue, state->queued_bytes, bytes, state->config)) {
                    auto oldest_log = std::find_if(
                        state->queue.begin(), state->queue.end(), [](const auto& queued) {
                            return std::holds_alternative<LoggerState::LogItem>(queued);
                        });
                    if (oldest_log == state->queue.end()) {
                        ++state->stats_value.records_dropped;
                        ++state->stats_value.discard_count;
                        state->stats_value.bytes_dropped += bytes;
                        writer.release_locked();
                        return;
                    }
                    const auto& evicted = std::get<LoggerState::LogItem>(*oldest_log);
                    state->queued_bytes -= record_bytes(evicted.record);
                    state->stats_value.bytes_dropped += record_bytes(evicted.record);
                    ++state->stats_value.records_dropped;
                    ++state->stats_value.overrun_count;
                    state->queue.erase(oldest_log);
                }
            } else {
                state->condition.wait(lock, [&state, bytes] {
                    return !state->accepting.load(std::memory_order_acquire) || state->stopping ||
                           state->backend_failed ||
                           can_fit_locked(state->queue, state->queued_bytes, bytes, state->config);
                });
                if (!state->accepting.load(std::memory_order_acquire) || state->stopping ||
                    state->backend_failed) {
                    ++state->stats_value.records_dropped;
                    state->stats_value.bytes_dropped += bytes;
                    writer.release_locked();
                    return;
                }
            }
        }
        record.sequence = state->next_sequence++;
        state->queue.emplace_back(LoggerState::LogItem{std::move(record)});
        state->queued_bytes += bytes;
        state->stats_value.queue_records_current = state->queue.size();
        state->stats_value.queue_bytes_current = state->queued_bytes;
        state->stats_value.queue_records_high_water = std::max<std::uint64_t>(
            state->stats_value.queue_records_high_water, state->queue.size());
        state->stats_value.queue_bytes_high_water =
            std::max<std::uint64_t>(state->stats_value.queue_bytes_high_water, state->queued_bytes);
        writer.release_locked();
        state->condition.notify_one();
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(state->mutex);
            ++state->stats_value.records_dropped;
            state->stats_value.bytes_dropped += record_bytes(category_, message);
        } catch (...) {
        }
    }
}

LoggingStats Logger::stats() const noexcept {
    auto state = state_.lock();
    return state == nullptr ? LoggingStats{} : state->stats();
}

Result<void> Logger::request_flush() {
    auto state = state_.lock();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is not initialized");
    }
    if (!state->config.async.enabled) {
        try {
            auto result = state->backend->flush();
            if (!result.ok()) {
                state->record_backend_failure();
            }
            return result;
        } catch (const std::exception& error) {
            state->record_backend_failure();
            return Status(StatusCode::Unavailable, "logging", 0, error.what());
        } catch (...) {
            state->record_backend_failure();
            return Status(StatusCode::Unavailable, "logging", 0, "unknown logging flush failure");
        }
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->condition.wait(lock, [&state] {
        return !state->accepting.load(std::memory_order_acquire) || state->stopping ||
               state->queue.size() < state->config.async.queue_records;
    });
    if (!state->accepting.load(std::memory_order_acquire) || state->stopping) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is stopping");
    }
    state->queue.emplace_back(LoggerState::FlushItem{});
    state->condition.notify_one();
    return {};
}

Result<void> Logger::enable_backtrace(std::size_t capacity) {
    if (capacity == 0 || capacity > kMaxBacktraceCapacity) {
        return Status(
            StatusCode::InvalidArgument, "logging", 0, "backtrace capacity must be in [1, 65536]");
    }
    auto state = state_.lock();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is not initialized");
    }
    try {
        auto result = state->backend->enable_backtrace(capacity);
        if (!result.ok()) {
            state->record_backend_failure();
            return result;
        }
    } catch (const std::exception& error) {
        state->record_backend_failure();
        return Status(
            StatusCode::Unavailable, "logging", 0,
            std::string("failed to enable backtrace: ") + error.what());
    } catch (...) {
        state->record_backend_failure();
        return Status(StatusCode::Unavailable, "logging", 0, "unknown backtrace failure");
    }
    state->backtrace_enabled.store(true, std::memory_order_release);
    return {};
}

Result<void> Logger::disable_backtrace() {
    auto state = state_.lock();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is not initialized");
    }
    try {
        auto result = state->backend->disable_backtrace();
        if (!result.ok()) {
            state->record_backend_failure();
            return result;
        }
    } catch (const std::exception& error) {
        state->record_backend_failure();
        return Status(
            StatusCode::Unavailable, "logging", 0,
            std::string("failed to disable backtrace: ") + error.what());
    } catch (...) {
        state->record_backend_failure();
        return Status(StatusCode::Unavailable, "logging", 0, "unknown backtrace failure");
    }
    state->backtrace_enabled.store(false, std::memory_order_release);
    return {};
}

Result<void> Logger::dump_backtrace() {
    auto state = state_.lock();
    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is not initialized");
    }
    if (!state->config.async.enabled) {
        try {
            auto result = state->backend->dump_backtrace();
            if (!result.ok()) {
                state->record_backend_failure();
            }
            return result;
        } catch (const std::exception& error) {
            state->record_backend_failure();
            return Status(StatusCode::Unavailable, "logging", 0, error.what());
        } catch (...) {
            state->record_backend_failure();
            return Status(StatusCode::Unavailable, "logging", 0, "unknown backtrace failure");
        }
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->condition.wait(lock, [&state] {
        return !state->accepting.load(std::memory_order_acquire) || state->stopping ||
               state->queue.size() < state->config.async.queue_records;
    });
    if (!state->accepting.load(std::memory_order_acquire) || state->stopping) {
        return Status(StatusCode::InvalidState, "logging", 0, "logging is stopping");
    }
    state->queue.emplace_back(LoggerState::DumpBacktraceItem{});
    state->condition.notify_one();
    return {};
}

}  // namespace rodaq
