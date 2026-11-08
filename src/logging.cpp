#include "rodaq/logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/version.h>

#if SPDLOG_VERSION != SPDLOG_TO_VERSION(1, 17, 0)
#error "RoDAQ logging requires spdlog 1.17.0 exactly"
#endif

#ifndef SPDLOG_COMPILED_LIB
#error "RoDAQ logging requires the compiled spdlog target"
#endif

#ifndef SPDLOG_FMT_EXTERNAL
#error "RoDAQ logging requires spdlog to use the external fmt target"
#endif

#ifdef SPDLOG_FMT_EXTERNAL_HO
#error "RoDAQ logging does not support header-only external fmt"
#endif

#ifdef SPDLOG_USE_STD_FORMAT
#error "RoDAQ logging does not use std::format"
#endif

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace rodaq::logging {
namespace {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

constexpr std::size_t kMaxAsyncQueueSize = 262144;
constexpr std::size_t kMaxBacktraceCapacity = 65536;
constexpr std::size_t kAsyncWorkerCount = 1;
constexpr Level kFallbackLevel = Level::Info;
constexpr auto kBackendErrorReportInterval = std::chrono::seconds{1};

// -----------------------------------------------------------------------------
// Logging state and lifecycle guard
// -----------------------------------------------------------------------------

struct LoggingState final {
    LoggingState() = default;

    LoggingState(const LoggingState&) = delete;
    LoggingState(LoggingState&&) = delete;

    LoggingState& operator=(const LoggingState&) = delete;
    LoggingState& operator=(LoggingState&&) = delete;

    ~LoggingState() noexcept {
        // Defensive lifecycle cleanup only. Normal shutdown stops and joins the
        // periodic flush thread explicitly before draining the logging backend.
        stop_flush.store(true, std::memory_order_release);
        flush_cv.notify_all();

        if (flush_thread.joinable()) {
            flush_thread.join();
        }
    }

    // Declaration order is intentional for the fallback RAII destruction path:
    // members are destroyed in reverse order, so logger references disappear
    // before the private thread pool, and the pool disappears before sinks.
    // shutdown() uses an explicit sequence instead: it first releases the
    // wrapper-owned logger references, then destroys the private pool to drain
    // surviving queued records, and finally flushes the still-owned sinks.
    std::vector<spdlog::sink_ptr> sinks;
    std::shared_ptr<spdlog::details::thread_pool> thread_pool;
    std::shared_ptr<spdlog::async_logger> async_logger;
    std::shared_ptr<spdlog::logger> logger;

    // accepting gates acquisition of new StateLease instances.
    std::atomic<bool> accepting{true};
    // closing is set only by shutdown() and coordinates the final
    // in-flight notification; it is not simply the inverse of accepting.
    std::atomic<bool> closing{false};
    std::atomic<bool> backtrace_enabled{false};
    std::atomic<bool> stop_flush{false};
    std::atomic<std::uint64_t> inflight_count{0};
    std::atomic<std::uint64_t> backend_error_count{0};

    bool async_enabled{false};
    std::size_t queue_capacity{0};

    std::mutex inflight_mutex;
    std::condition_variable inflight_cv;

    std::mutex flush_mutex;
    std::condition_variable flush_cv;
    std::thread flush_thread;
    std::chrono::milliseconds flush_interval{0};

    std::mutex backend_error_mutex;
    std::chrono::steady_clock::time_point last_backend_error_report{};
};

std::mutex lifecycle_mutex;
std::mutex fallback_mutex;
std::shared_ptr<LoggingState> logging_state;
std::atomic<Level> current_level{kFallbackLevel};

// C++20+: migrate these free atomic shared_ptr operations to
// std::atomic<std::shared_ptr<LoggingState>> when the project language baseline
// is raised.
std::shared_ptr<LoggingState> load_state() noexcept {
    return std::atomic_load_explicit(&logging_state, std::memory_order_acquire);
}

void store_state(std::shared_ptr<LoggingState> state) noexcept {
    std::atomic_store_explicit(&logging_state, std::move(state), std::memory_order_release);
}

std::shared_ptr<LoggingState> take_state() noexcept {
    return std::atomic_exchange_explicit(
        &logging_state, std::shared_ptr<LoggingState>{}, std::memory_order_acq_rel);
}

void release_state_use(LoggingState& state) noexcept {
    const auto previous = state.inflight_count.fetch_sub(1, std::memory_order_acq_rel);

    if (previous != 1) {
        return;
    }

    // The mutex is only taken on the last in-flight release while shutdown is
    // waiting. The seq_cst closing flag prevents the notify from being lost
    // between the shutdown predicate check and condition-variable wait.
    if (!state.closing.load(std::memory_order_seq_cst)) {
        return;
    }

    std::lock_guard<std::mutex> lock{state.inflight_mutex};
    state.inflight_cv.notify_all();
}

class StateLease final {
public:
    StateLease() = default;

    explicit StateLease(std::shared_ptr<LoggingState> state) : state_(std::move(state)) {}

    StateLease(const StateLease&) = delete;
    StateLease& operator=(const StateLease&) = delete;

    StateLease(StateLease&& other) noexcept : state_(std::move(other.state_)) {}

    StateLease& operator=(StateLease&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
        }

        return *this;
    }

    ~StateLease() { reset(); }

    explicit operator bool() const noexcept { return state_ != nullptr; }

    LoggingState* operator->() noexcept { return state_.get(); }

    const LoggingState* operator->() const noexcept { return state_.get(); }

    LoggingState& operator*() noexcept { return *state_; }

private:
    void reset() noexcept {
        if (state_ == nullptr) {
            return;
        }

        release_state_use(*state_);
        state_.reset();
    }

    std::shared_ptr<LoggingState> state_;
};

StateLease acquire_state() noexcept {
    auto state = load_state();

    if (state == nullptr || !state->accepting.load(std::memory_order_acquire)) {
        return {};
    }

    state->inflight_count.fetch_add(1, std::memory_order_acq_rel);

    if (!state->accepting.load(std::memory_order_acquire)) {
        release_state_use(*state);
        return {};
    }

    return StateLease{std::move(state)};
}

void wait_for_state_users(LoggingState& state) {
    std::unique_lock<std::mutex> lock{state.inflight_mutex};

    state.inflight_cv.wait(
        lock, [&state] { return state.inflight_count.load(std::memory_order_acquire) == 0; });
}

// -----------------------------------------------------------------------------
// Backend enum conversion
// -----------------------------------------------------------------------------

bool is_valid_level(Level level) noexcept {
    return static_cast<std::uint8_t>(level) <= static_cast<std::uint8_t>(Level::Off);
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

// These switches intentionally omit default labels so compiler exhaustiveness
// diagnostics remain useful when project enums grow. The trailing return is a
// defensive fallback for invalid values produced by explicit casts.
spdlog::level::level_enum to_backend_level(Level level) noexcept {
    switch (level) {
        case Level::Trace:
            return spdlog::level::trace;
        case Level::Debug:
            return spdlog::level::debug;
        case Level::Info:
            return spdlog::level::info;
        case Level::Warning:
            return spdlog::level::warn;
        case Level::Error:
            return spdlog::level::err;
        case Level::Critical:
            return spdlog::level::critical;
        case Level::Off:
            return spdlog::level::off;
    }

    return spdlog::level::off;
}

spdlog::pattern_time_type to_backend_time_zone(TimeZone time_zone) noexcept {
    switch (time_zone) {
        case TimeZone::Local:
            return spdlog::pattern_time_type::local;
        case TimeZone::Utc:
            return spdlog::pattern_time_type::utc;
    }

    return spdlog::pattern_time_type::local;
}

spdlog::async_overflow_policy to_backend_overflow_policy(OverflowPolicy policy) noexcept {
    switch (policy) {
        case OverflowPolicy::Block:
            return spdlog::async_overflow_policy::block;
        case OverflowPolicy::OverrunOldest:
            return spdlog::async_overflow_policy::overrun_oldest;
        case OverflowPolicy::DiscardNew:
            return spdlog::async_overflow_policy::discard_new;
    }

    return spdlog::async_overflow_policy::overrun_oldest;
}

bool is_level_enabled(Level message_level, Level threshold) noexcept {
    if (!is_valid_level(message_level) || !is_valid_level(threshold) ||
        message_level == Level::Off || threshold == Level::Off) {
        return false;
    }

    return static_cast<std::uint8_t>(message_level) >= static_cast<std::uint8_t>(threshold);
}

const char* level_name(Level level) noexcept {
    switch (level) {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warning:
            return "warning";
        case Level::Error:
            return "error";
        case Level::Critical:
            return "critical";
        case Level::Off:
            return "off";
    }

    return "unknown";
}

// -----------------------------------------------------------------------------
// Fallback diagnostics
// -----------------------------------------------------------------------------

void write_fallback_unlocked(
    Level level, SourceLocation source, std::string_view message) noexcept {
    std::fprintf(stderr, "[rodaq] [%s]", level_name(level));

    if (source.file != nullptr && source.line > 0) {
        std::fprintf(stderr, " [%s:%d]", source.file, source.line);
    }

    std::fputc(' ', stderr);

    if (!message.empty()) {
        std::fwrite(message.data(), sizeof(char), message.size(), stderr);
    }

    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void write_fallback(Level level, SourceLocation source, std::string_view message) noexcept {
    try {
        std::lock_guard<std::mutex> lock{fallback_mutex};
        write_fallback_unlocked(level, source, message);
    } catch (...) {
        // Last-resort path: if synchronization itself fails, preserve diagnostics
        // rather than propagating an exception through a noexcept logging API.
        write_fallback_unlocked(level, source, message);
    }
}

void record_backend_error(LoggingState& state, std::string_view message) noexcept {
    state.backend_error_count.fetch_add(1, std::memory_order_relaxed);

    bool report = false;

    try {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock{state.backend_error_mutex};

        if (state.last_backend_error_report == std::chrono::steady_clock::time_point{} ||
            now - state.last_backend_error_report >= kBackendErrorReportInterval) {
            state.last_backend_error_report = now;
            report = true;
        }
    } catch (...) {
        report = true;
    }

    if (report) {
        write_fallback(Level::Error, {}, message);
    }
}

Status make_status(StatusCode code, std::string_view description, std::string_view detail = {}) {
    std::string message;
    message.reserve(description.size() + (detail.empty() ? 0U : detail.size() + 2U));

    message.append(description.data(), description.size());

    // An empty detail intentionally produces no trailing separator.
    if (!detail.empty()) {
        message.append(": ");
        message.append(detail.data(), detail.size());
    }

    return Status{code, std::move(message)};
}

// -----------------------------------------------------------------------------
// Configuration validation
// -----------------------------------------------------------------------------

struct SinkValidationState final {
    bool stdout_used{false};
    bool stderr_used{false};
    std::set<std::string> file_paths;
};

std::string normalize_log_path(const std::string& path) {
    const std::filesystem::path input{path};

    std::error_code error;
    auto absolute = std::filesystem::absolute(input, error);

    if (error) {
        absolute = input;
    }

    return absolute.lexically_normal().generic_string();
}

Result<void> validate_pattern(const std::string& pattern, TimeZone time_zone) {
    if (pattern.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "logging pattern must not be empty",
        };
    }

    try {
        // Construction parses and validates the pattern. The formatter instance
        // itself is intentionally discarded after validation.
        spdlog::pattern_formatter formatter{pattern, to_backend_time_zone(time_zone)};
        (void)formatter;
        return {};
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::InvalidArgument, "invalid logging pattern", exception.what());
    } catch (...) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid logging pattern",
        };
    }
}

Result<void> validate_sink_options(const SinkOptions& options, const Config& config) {
    if (!is_valid_level(options.level)) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid logging sink level",
        };
    }

    if (options.pattern.empty()) {
        return {};
    }

    return validate_pattern(options.pattern, config.time_zone);
}

Result<void> record_file_sink_path(const std::string& path, SinkValidationState& state) {
    const auto normalized = normalize_log_path(path);

    if (!state.file_paths.insert(normalized).second) {
        return Status{
            StatusCode::Conflict,
            "multiple logging sinks use the same file path",
        };
    }

    return {};
}

Result<void> validate_sink(
    const ConsoleSinkConfig& sink_config, const Config& config, SinkValidationState& state) {
    auto options_result = validate_sink_options(sink_config.options, config);
    if (!options_result.ok()) {
        return std::move(options_result).status();
    }

    if (!is_valid_console_stream(sink_config.stream)) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid console logging stream",
        };
    }

    bool* used = nullptr;

    switch (sink_config.stream) {
        case ConsoleStream::Stdout:
            used = &state.stdout_used;
            break;
        case ConsoleStream::Stderr:
            used = &state.stderr_used;
            break;
    }

    // Defensive fallback for future enum extensions that are not handled by
    // the switch above. Current validated values cannot reach this branch.
    if (used == nullptr) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid console logging stream",
        };
    }

    if (*used) {
        return Status{
            StatusCode::Conflict,
            "multiple logging sinks use the same console stream",
        };
    }

    *used = true;
    return {};
}

Result<void> validate_sink(
    const FileSinkConfig& sink_config, const Config& config, SinkValidationState& state) {
    auto options_result = validate_sink_options(sink_config.options, config);
    if (!options_result.ok()) {
        return std::move(options_result).status();
    }

    if (sink_config.path.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "file logging path must not be empty",
        };
    }

    return record_file_sink_path(sink_config.path, state);
}

Result<void> validate_sink(
    const RotatingFileSinkConfig& sink_config, const Config& config, SinkValidationState& state) {
    auto options_result = validate_sink_options(sink_config.options, config);
    if (!options_result.ok()) {
        return std::move(options_result).status();
    }

    if (sink_config.path.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "rotating log path must not be empty",
        };
    }

    if (sink_config.max_size == 0) {
        return Status{
            StatusCode::InvalidArgument,
            "rotating log max_size must be greater than zero",
        };
    }

    if (sink_config.max_files == 0) {
        return Status{
            StatusCode::InvalidArgument,
            "rotating log max_files must be greater than zero",
        };
    }

    return record_file_sink_path(sink_config.path, state);
}

Result<void> validate_sink(
    const DailyFileSinkConfig& sink_config, const Config& config, SinkValidationState& state) {
    auto options_result = validate_sink_options(sink_config.options, config);
    if (!options_result.ok()) {
        return std::move(options_result).status();
    }

    if (sink_config.path.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "daily log path must not be empty",
        };
    }

    if (sink_config.rotation_hour > 23) {
        return Status{
            StatusCode::InvalidArgument,
            "daily log rotation_hour must be in [0, 23]",
        };
    }

    if (sink_config.rotation_minute > 59) {
        return Status{
            StatusCode::InvalidArgument,
            "daily log rotation_minute must be in [0, 59]",
        };
    }

    return record_file_sink_path(sink_config.path, state);
}

Result<void> validate_config(const Config& config) {
    if (config.name.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "logging name must not be empty",
        };
    }

    if (!is_valid_level(config.level)) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid logging level",
        };
    }

    if (!is_valid_time_zone(config.time_zone)) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid logging time zone",
        };
    }

    if (!is_valid_overflow_policy(config.async.overflow_policy)) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid asynchronous logging overflow policy",
        };
    }

    if (!is_valid_level(config.flush.level)) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid logging flush level",
        };
    }

    if (config.sinks.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "at least one logging sink is required",
        };
    }

    auto pattern_result = validate_pattern(config.pattern, config.time_zone);
    if (!pattern_result.ok()) {
        return std::move(pattern_result).status();
    }

    if (config.async.enabled &&
        (config.async.queue_size == 0 || config.async.queue_size > kMaxAsyncQueueSize)) {
        return Status{
            StatusCode::InvalidArgument,
            "async logging queue_size must be in [1, 262144]",
        };
    }

    if (config.flush.interval.count() < 0) {
        return Status{
            StatusCode::InvalidArgument,
            "logging flush interval must not be negative",
        };
    }

    if (config.backtrace_capacity > kMaxBacktraceCapacity) {
        return Status{
            StatusCode::InvalidArgument,
            "logging backtrace_capacity must be <= 65536",
        };
    }

    SinkValidationState validation_state;

    for (const auto& sink_config : config.sinks) {
        auto sink_result = std::visit(
            [&](const auto& value) { return validate_sink(value, config, validation_state); },
            sink_config);

        if (!sink_result.ok()) {
            return std::move(sink_result).status();
        }
    }

    return {};
}

// -----------------------------------------------------------------------------
// Sink construction
// -----------------------------------------------------------------------------

void configure_sink(
    const spdlog::sink_ptr& sink, const SinkOptions& options, const Config& config) {
    sink->set_level(to_backend_level(options.level));

    const auto& pattern = options.pattern.empty() ? config.pattern : options.pattern;

    sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(
        pattern, to_backend_time_zone(config.time_zone)));
}

Result<spdlog::sink_ptr> create_sink(const ConsoleSinkConfig& sink_config, const Config& config) {
    try {
        spdlog::sink_ptr sink;

        if (sink_config.color) {
            if (sink_config.stream == ConsoleStream::Stdout) {
                sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            } else {
                sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            }
        } else {
            if (sink_config.stream == ConsoleStream::Stdout) {
                sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
            } else {
                sink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
            }
        }

        configure_sink(sink, sink_config.options, config);
        return spdlog::sink_ptr{std::move(sink)};
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "failed to allocate console logging sink",
        };
    } catch (const spdlog::spdlog_ex& exception) {
        return make_status(
            StatusCode::Unavailable, "failed to create console logging sink", exception.what());
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::Internal, "unexpected console logging sink failure", exception.what());
    } catch (...) {
        return Status{
            StatusCode::Internal,
            "unknown console logging sink failure",
        };
    }
}

Result<spdlog::sink_ptr> create_sink(const FileSinkConfig& sink_config, const Config& config) {
    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            sink_config.path, sink_config.truncate);

        configure_sink(sink, sink_config.options, config);
        return spdlog::sink_ptr{std::move(sink)};
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "failed to allocate file logging sink",
        };
    } catch (const spdlog::spdlog_ex& exception) {
        return make_status(
            StatusCode::Unavailable, "failed to create file logging sink", exception.what());
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::Unavailable, "unexpected file logging sink failure", exception.what());
    } catch (...) {
        return Status{
            StatusCode::Unavailable,
            "unknown file logging sink failure",
        };
    }
}

Result<spdlog::sink_ptr> create_sink(
    const RotatingFileSinkConfig& sink_config, const Config& config) {
    try {
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            sink_config.path, sink_config.max_size, sink_config.max_files,
            sink_config.rotate_on_open);

        configure_sink(sink, sink_config.options, config);
        return spdlog::sink_ptr{std::move(sink)};
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "failed to allocate rotating file logging sink",
        };
    } catch (const spdlog::spdlog_ex& exception) {
        return make_status(
            StatusCode::Unavailable, "failed to create rotating file logging sink",
            exception.what());
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::Unavailable, "unexpected rotating file logging sink failure",
            exception.what());
    } catch (...) {
        return Status{
            StatusCode::Unavailable,
            "unknown rotating file logging sink failure",
        };
    }
}

Result<spdlog::sink_ptr> create_sink(const DailyFileSinkConfig& sink_config, const Config& config) {
    try {
        auto sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            sink_config.path, static_cast<int>(sink_config.rotation_hour),
            static_cast<int>(sink_config.rotation_minute), sink_config.truncate,
            sink_config.max_files);

        configure_sink(sink, sink_config.options, config);
        return spdlog::sink_ptr{std::move(sink)};
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "failed to allocate daily file logging sink",
        };
    } catch (const spdlog::spdlog_ex& exception) {
        return make_status(
            StatusCode::Unavailable, "failed to create daily file logging sink", exception.what());
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::Unavailable, "unexpected daily file logging sink failure",
            exception.what());
    } catch (...) {
        return Status{
            StatusCode::Unavailable,
            "unknown daily file logging sink failure",
        };
    }
}

Result<std::vector<spdlog::sink_ptr>> create_sinks(const Config& config) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.reserve(config.sinks.size());

    for (const auto& sink_config : config.sinks) {
        auto sink_result =
            std::visit([&](const auto& value) { return create_sink(value, config); }, sink_config);

        if (!sink_result.ok()) {
            return std::move(sink_result).status();
        }

        sinks.emplace_back(std::move(sink_result).value());
    }

    return Result<std::vector<spdlog::sink_ptr>>{std::move(sinks)};
}

// -----------------------------------------------------------------------------
// Flush and shutdown helpers
// -----------------------------------------------------------------------------

void periodic_flush_no_throw(LoggingState& state) noexcept {
    for (const auto& sink : state.sinks) {
        try {
            sink->flush();
        } catch (const std::exception& exception) {
            record_backend_error(state, exception.what());
        } catch (...) {
            record_backend_error(state, "unknown periodic logging flush failure");
        }
    }
}

void start_periodic_flush(LoggingState& state) {
    if (state.flush_interval.count() <= 0) {
        return;
    }

    state.flush_thread = std::thread{[&state] {
        std::unique_lock<std::mutex> lock{state.flush_mutex};

        while (!state.stop_flush.load(std::memory_order_acquire)) {
            const bool stopped = state.flush_cv.wait_for(lock, state.flush_interval, [&state] {
                return state.stop_flush.load(std::memory_order_acquire);
            });

            if (stopped) {
                break;
            }

            lock.unlock();
            periodic_flush_no_throw(state);
            lock.lock();
        }
    }};
}

void stop_periodic_flush(LoggingState& state) {
    state.stop_flush.store(true, std::memory_order_release);
    state.flush_cv.notify_all();

    if (state.flush_thread.joinable()) {
        state.flush_thread.join();
    }
}

Result<void> flush_sinks(LoggingState& state) {
    // Default-constructed Status is Ok and means no sink flush error has been
    // recorded yet.
    Status first_error;

    for (const auto& sink : state.sinks) {
        try {
            sink->flush();
        } catch (const std::exception& exception) {
            record_backend_error(state, exception.what());

            if (first_error.ok()) {
                first_error = make_status(
                    StatusCode::Unavailable, "failed to flush logging sink", exception.what());
            }
        } catch (...) {
            record_backend_error(state, "unknown logging sink flush failure");

            if (first_error.ok()) {
                first_error = Status{StatusCode::Unavailable, "unknown logging sink flush failure"};
            }
        }
    }

    if (!first_error.ok()) {
        return first_error;
    }

    return {};
}

Result<void> request_backend_flush(LoggingState& state) {
    try {
        if (state.async_enabled) {
            if (state.thread_pool == nullptr || state.async_logger == nullptr) {
                return Status{
                    StatusCode::Internal,
                    "asynchronous logging state is incomplete",
                };
            }

            auto logger = state.async_logger;
            state.thread_pool->post_flush(std::move(logger), spdlog::async_overflow_policy::block);

            return {};
        }

        if (state.logger == nullptr) {
            return Status{
                StatusCode::Internal,
                "logging state has no logger",
            };
        }

        const auto errors_before = state.backend_error_count.load(std::memory_order_relaxed);

        state.logger->flush();

        const auto errors_after = state.backend_error_count.load(std::memory_order_relaxed);

        if (errors_after != errors_before) {
            return Status{
                StatusCode::Unavailable,
                "logging backend reported an error while flushing",
            };
        }

        return {};
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "logging flush request exhausted memory",
        };
    } catch (const std::exception& exception) {
        record_backend_error(state, exception.what());
        return make_status(
            StatusCode::Unavailable, "failed to request logging flush", exception.what());
    } catch (...) {
        record_backend_error(state, "unknown logging flush request failure");
        return Status{
            StatusCode::Internal,
            "unknown logging flush request failure",
        };
    }
}

// -----------------------------------------------------------------------------
// State construction
// -----------------------------------------------------------------------------

Result<std::shared_ptr<LoggingState>> create_logging_state(const Config& config) {
    auto sinks_result = create_sinks(config);
    if (!sinks_result.ok()) {
        return std::move(sinks_result).status();
    }

    try {
        auto state = std::make_shared<LoggingState>();

        state->sinks = std::move(sinks_result).value();
        state->async_enabled = config.async.enabled;
        state->queue_capacity = config.async.enabled ? config.async.queue_size : 0;
        state->flush_interval = config.flush.interval;

        if (config.async.enabled) {
            state->thread_pool = std::make_shared<spdlog::details::thread_pool>(
                config.async.queue_size, kAsyncWorkerCount);

            state->async_logger = std::make_shared<spdlog::async_logger>(
                config.name, state->sinks.begin(), state->sinks.end(), state->thread_pool,
                to_backend_overflow_policy(config.async.overflow_policy));

            // Keep the derived async_logger for ordered post_flush() requests while
            // exposing the common logger interface to the rest of the implementation.
            state->logger = state->async_logger;
        } else {
            state->logger = std::make_shared<spdlog::logger>(
                config.name, state->sinks.begin(), state->sinks.end());
        }

        std::weak_ptr<LoggingState> weak_state{state};

        // spdlog documents set_error_handler() as non-thread-safe. Configure it
        // before publishing the state to application threads.
        state->logger->set_error_handler([weak_state](const std::string& message) noexcept {
            if (auto active = weak_state.lock()) {
                record_backend_error(*active, message);
                return;
            }

            write_fallback(Level::Error, {}, message);
        });

        state->logger->set_level(to_backend_level(config.level));
        state->logger->flush_on(to_backend_level(config.flush.level));

        if (config.backtrace_capacity > 0) {
            state->logger->enable_backtrace(config.backtrace_capacity);
            state->backtrace_enabled.store(true, std::memory_order_release);
        }

        start_periodic_flush(*state);

        return state;
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "failed to allocate logging runtime resources",
        };
    } catch (const std::system_error& exception) {
        return make_status(
            StatusCode::ResourceExhausted, "failed to create logging worker resources",
            exception.what());
    } catch (const spdlog::spdlog_ex& exception) {
        return make_status(
            StatusCode::Unavailable, "failed to initialize logging backend", exception.what());
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::Internal, "unexpected logging initialization failure", exception.what());
    } catch (...) {
        return Status{
            StatusCode::Internal,
            "unknown logging initialization failure",
        };
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

Result<void> initialize(const Config& config) {
    std::lock_guard<std::mutex> lock{lifecycle_mutex};

    if (load_state() != nullptr) {
        return Status{
            StatusCode::Conflict,
            "logging is already initialized",
        };
    }

    try {
        auto validation_result = validate_config(config);
        if (!validation_result.ok()) {
            return std::move(validation_result).status();
        }

        auto state_result = create_logging_state(config);
        if (!state_result.ok()) {
            return std::move(state_result).status();
        }

        auto state = std::move(state_result).value();

        current_level.store(config.level, std::memory_order_release);

        store_state(std::move(state));
        return {};
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "logging initialization exhausted memory",
        };
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::Internal, "unexpected logging initialization failure", exception.what());
    } catch (...) {
        return Status{
            StatusCode::Internal,
            "unknown logging initialization failure",
        };
    }
}

Result<void> shutdown() {
    std::lock_guard<std::mutex> lock{lifecycle_mutex};

    auto state = take_state();
    if (state == nullptr) {
        return {};
    }

    state->accepting.store(false, std::memory_order_release);
    state->backtrace_enabled.store(false, std::memory_order_release);
    state->closing.store(true, std::memory_order_seq_cst);

    const auto errors_before = state->backend_error_count.load(std::memory_order_relaxed);

    stop_periodic_flush(*state);
    wait_for_state_users(*state);

    // Once all in-flight control operations are complete, restore the fixed
    // fallback threshold used while no logger is initialized.
    current_level.store(kFallbackLevel, std::memory_order_release);

    if (state->async_enabled) {
        auto thread_pool = std::move(state->thread_pool);

        // Release wrapper-owned logger references first. Queue records retain the
        // originating async logger as needed while the private single-worker pool
        // drains surviving messages.
        state->logger.reset();
        state->async_logger.reset();

        // spdlog's private pool destructor enqueues a blocking terminate record and
        // joins the worker. With one worker, surviving records already in the queue
        // are processed before termination. This can block indefinitely if backend
        // I/O is pathologically stalled; the public contract explicitly permits
        // unbounded shutdown latency.
        thread_pool.reset();
    } else {
        state->logger.reset();
    }

    auto flush_result = flush_sinks(*state);

    const auto errors_after = state->backend_error_count.load(std::memory_order_relaxed);

    if (!flush_result.ok()) {
        return std::move(flush_result).status();
    }

    if (errors_after != errors_before) {
        return Status{
            StatusCode::Unavailable,
            "logging backend reported errors while shutting down",
        };
    }

    return {};
}

Result<void> set_level(Level new_level) {
    if (!is_valid_level(new_level)) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid logging level",
        };
    }

    auto state = acquire_state();
    if (!state) {
        return Status{
            StatusCode::InvalidState,
            "logging is not initialized",
        };
    }

    state->logger->set_level(to_backend_level(new_level));
    current_level.store(new_level, std::memory_order_release);
    return {};
}

Result<void> enable_backtrace(std::size_t capacity) {
    if (capacity == 0 || capacity > kMaxBacktraceCapacity) {
        return Status{
            StatusCode::InvalidArgument,
            "backtrace capacity must be in [1, 65536]",
        };
    }

    auto state = acquire_state();
    if (!state) {
        return Status{
            StatusCode::InvalidState,
            "logging is not initialized",
        };
    }

    try {
        state->logger->enable_backtrace(capacity);
        state->backtrace_enabled.store(true, std::memory_order_release);
        return {};
    } catch (const std::bad_alloc&) {
        return Status{
            StatusCode::ResourceExhausted,
            "failed to allocate log backtrace buffer",
        };
    } catch (const std::exception& exception) {
        return make_status(
            StatusCode::Unavailable, "failed to enable log backtrace", exception.what());
    } catch (...) {
        return Status{
            StatusCode::Unavailable,
            "unknown log backtrace enable failure",
        };
    }
}

Result<void> disable_backtrace() {
    auto state = acquire_state();
    if (!state) {
        return Status{
            StatusCode::InvalidState,
            "logging is not initialized",
        };
    }

    try {
        state->logger->disable_backtrace();
        state->backtrace_enabled.store(false, std::memory_order_release);
        return {};
    } catch (const std::exception& exception) {
        record_backend_error(*state, exception.what());
        return make_status(
            StatusCode::Unavailable, "failed to disable log backtrace", exception.what());
    } catch (...) {
        record_backend_error(*state, "unknown log backtrace disable failure");
        return Status{
            StatusCode::Unavailable,
            "unknown log backtrace disable failure",
        };
    }
}

Result<void> dump_backtrace() {
    auto state = acquire_state();
    if (!state) {
        return Status{
            StatusCode::InvalidState,
            "logging is not initialized",
        };
    }

    try {
        state->logger->dump_backtrace();
        return {};
    } catch (const std::exception& exception) {
        record_backend_error(*state, exception.what());
        return make_status(
            StatusCode::Unavailable, "failed to dump log backtrace", exception.what());
    } catch (...) {
        record_backend_error(*state, "unknown log backtrace dump failure");
        return Status{
            StatusCode::Unavailable,
            "unknown log backtrace dump failure",
        };
    }
}

Result<void> request_flush() {
    auto state = acquire_state();

    if (!state) {
        return Status{
            StatusCode::InvalidState,
            "logging is not initialized",
        };
    }

    return request_backend_flush(*state);
}

Level level() noexcept { return current_level.load(std::memory_order_acquire); }

Stats stats() noexcept {
    Stats stats;

    auto state = acquire_state();
    if (!state) {
        return stats;
    }

    stats.initialized = true;
    stats.asynchronous = state->async_enabled;
    stats.backend_error_count = state->backend_error_count.load(std::memory_order_relaxed);

    if (state->thread_pool == nullptr) {
        return stats;
    }

    stats.queue_capacity = state->queue_capacity;

    try {
        stats.queue_size = state->thread_pool->queue_size();
        stats.overrun_count = static_cast<std::uint64_t>(state->thread_pool->overrun_counter());
        stats.discard_count = static_cast<std::uint64_t>(state->thread_pool->discard_counter());
    } catch (...) {
        // Preserve the valid partial snapshot already collected. A statistics read
        // must never terminate application code merely because the asynchronous queue
        // statistics could not be read.
    }

    return stats;
}

bool should_log(Level message_level) noexcept {
    if (!is_valid_level(message_level) || message_level == Level::Off) {
        return false;
    }

    auto state = load_state();

    if (state != nullptr && state->accepting.load(std::memory_order_acquire) &&
        state->backtrace_enabled.load(std::memory_order_acquire)) {
        return true;
    }

    return is_level_enabled(message_level, level());
}

void write(Level message_level, SourceLocation source, std::string_view message) noexcept {
    if (!is_valid_level(message_level) || message_level == Level::Off) {
        return;
    }

    auto state = acquire_state();

    if (!state) {
        if (is_level_enabled(message_level, level())) {
            write_fallback(message_level, source, message);
        }

        return;
    }

    try {
        const spdlog::source_loc backend_source{
            source.file != nullptr ? source.file : "", source.line > 0 ? source.line : 0,
            source.function != nullptr ? source.function : ""};

        const spdlog::string_view_t backend_message{message.data(), message.size()};

        // spdlog applies logger-level filtering and backtrace capture. Keeping
        // that decision in the logger preserves Level::Off plus backtrace
        // semantics without duplicating spdlog's backtrace logic here.
        state->logger->log(backend_source, to_backend_level(message_level), backend_message);
    } catch (const std::exception& exception) {
        record_backend_error(*state, exception.what());
        write_fallback(message_level, source, message);
    } catch (...) {
        record_backend_error(*state, "unknown logging backend failure");
        write_fallback(message_level, source, message);
    }
}

}  // namespace rodaq::logging
