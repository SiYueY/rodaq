#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "rodaq/result.hpp"

#if FMT_VERSION != 110200
#error "RoDAQ logging requires fmt 11.2.0 exactly"
#endif

#define RODAQ_LOG_LEVEL_TRACE 0
#define RODAQ_LOG_LEVEL_DEBUG 1
#define RODAQ_LOG_LEVEL_INFO 2
#define RODAQ_LOG_LEVEL_WARNING 3
#define RODAQ_LOG_LEVEL_ERROR 4
#define RODAQ_LOG_LEVEL_CRITICAL 5
#define RODAQ_LOG_LEVEL_OFF 6

#ifdef RODAQ_ACTIVE_LOG_LEVEL
#error "RODAQ_ACTIVE_LOG_LEVEL was renamed to RODAQ_LOG_ACTIVE_LEVEL"
#endif

#ifndef RODAQ_LOG_ACTIVE_LEVEL
#define RODAQ_LOG_ACTIVE_LEVEL RODAQ_LOG_LEVEL_INFO
#endif

#if RODAQ_LOG_ACTIVE_LEVEL < RODAQ_LOG_LEVEL_TRACE || RODAQ_LOG_ACTIVE_LEVEL > RODAQ_LOG_LEVEL_OFF
#error "RODAQ_LOG_ACTIVE_LEVEL is invalid"
#endif

namespace rodaq::logging {

/**
 * @brief Severity level of a log message.
 *
 * Level::Off disables logger emission to configured sinks. If backtrace is
 * enabled, log records may still be captured by the backtrace ring buffer
 * even while the logger level is Off.
 *
 * RODAQ_LOG_ACTIVE_LEVEL is different: messages removed by compile-time
 * pruning do not exist at runtime and therefore cannot enter backtrace.
 */
enum class Level : std::uint8_t {
    Trace = RODAQ_LOG_LEVEL_TRACE,
    Debug = RODAQ_LOG_LEVEL_DEBUG,
    Info = RODAQ_LOG_LEVEL_INFO,
    Warning = RODAQ_LOG_LEVEL_WARNING,
    Error = RODAQ_LOG_LEVEL_ERROR,
    Critical = RODAQ_LOG_LEVEL_CRITICAL,
    Off = RODAQ_LOG_LEVEL_OFF,
};

/**
 * @brief Time zone used by text-pattern timestamp formatting.
 *
 * This setting affects pattern formatting only. It does not change the
 * rotation-time semantics of DailyFileSinkConfig.
 */
enum class TimeZone : std::uint8_t {
    Local,
    Utc,
};

/**
 * @brief Policy applied when the asynchronous queue is full.
 *
 * Block waits for queue capacity and can propagate logging backpressure to the
 * calling thread.
 *
 * OverrunOldest replaces the oldest queued item with the new item.
 *
 * DiscardNew drops the new item when no queue capacity is available.
 *
 * None of these policies makes logging hard-real-time safe.
 */
enum class OverflowPolicy : std::uint8_t {
    Block,
    OverrunOldest,
    DiscardNew,
};

/**
 * @brief Output stream used by a console sink.
 */
enum class ConsoleStream : std::uint8_t {
    Stdout,
    Stderr,
};

/**
 * @brief Source location associated with a log message.
 *
 * The pointed-to strings must remain valid for the duration of the logging
 * call. Source locations produced by the RODAQ_LOG_* macros satisfy this
 * requirement.
 */
struct SourceLocation final {
    const char* file{nullptr};
    int line{0};
    const char* function{nullptr};
};

/**
 * @brief Common sink configuration.
 *
 * level is the sink-level severity threshold. It is applied after the logger
 * level, so it can only filter messages that have already passed Config::level.
 * Level::Trace therefore means that the sink adds no additional severity
 * filtering.
 *
 * pattern is a sink-specific formatter pattern. An empty string means that the
 * sink uses Config::pattern.
 */
struct SinkOptions final {
    Level level{Level::Trace};
    std::string pattern;
};

/**
 * @brief Console sink configuration.
 */
struct ConsoleSinkConfig final {
    SinkOptions options;
    ConsoleStream stream{ConsoleStream::Stdout};
    bool color{true};
};

/**
 * @brief Non-rotating file sink configuration.
 *
 * Each configured file sink path must have single-process ownership. RoDAQ does
 * not provide inter-process synchronization for log files.
 */
struct FileSinkConfig final {
    SinkOptions options;
    std::string path;
    bool truncate{false};
};

/**
 * @brief Size-based rotating file sink configuration.
 *
 * The base file and its rotated files must have single-process ownership.
 */
struct RotatingFileSinkConfig final {
    SinkOptions options;
    std::string path;
    std::size_t max_size{50U * 1024U * 1024U};
    std::size_t max_files{5};
    bool rotate_on_open{false};
};

/**
 * @brief Daily rotating file sink configuration.
 *
 * max_files limits retention managed while the active backend is running. It
 * is not a complete historical cleanup guarantee for files left by previous
 * executions.
 *
 * Daily rotation follows the backend's local-time rotation semantics. The
 * Config::time_zone setting affects text formatting, not rotation time.
 *
 * The configured file sink path must have single-process ownership.
 */
struct DailyFileSinkConfig final {
    SinkOptions options;
    std::string path;
    std::uint8_t rotation_hour{0};
    std::uint8_t rotation_minute{0};
    bool truncate{false};

    /**
     * @brief Number of files retained by the active backend.
     *
     * Zero disables retention limiting.
     */
    std::uint16_t max_files{0};
};

/**
 * @brief Supported sink configurations.
 */
using SinkConfig =
    std::variant<ConsoleSinkConfig, FileSinkConfig, RotatingFileSinkConfig, DailyFileSinkConfig>;

/**
 * @brief Asynchronous logging configuration.
 *
 * RoDAQ logging uses exactly one asynchronous worker so queue
 * processing order is preserved.
 *
 * The asynchronous queue is allocated during initialization. queue_size must
 * be in [1, 262144] when asynchronous logging is enabled.
 *
 * Asynchronous logging is not hard-real-time safe and provides no bounded
 * worst-case producer latency.
 */
struct AsyncConfig final {
    bool enabled{true};
    std::size_t queue_size{8192};
    OverflowPolicy overflow_policy{OverflowPolicy::OverrunOldest};
};

/**
 * @brief Flush configuration.
 *
 * level is the logger flush-on-level threshold. Level::Off disables
 * level-triggered flushing.
 *
 * interval controls periodic sink flushing. A zero interval disables periodic
 * flushing.
 *
 * Sink flushing does not imply fsync or physical-media durability.
 */
struct FlushConfig final {
    Level level{Level::Error};
    std::chrono::milliseconds interval{0};
};

/**
 * @brief Logger configuration.
 *
 * Sink configuration, formatter patterns, asynchronous queue settings,
 * rotation settings, and overflow policy are immutable after successful
 * initialization. Runtime mutation is limited to the logger level and
 * backtrace controls.
 *
 * RoDAQ logging guarantees the following commonly used pattern fields:
 *
 * - %v message
 * - %l level
 * - %L short level
 * - %n logger name
 * - %Y %m %d date
 * - %H %M %S %e time
 * - %z timezone offset
 * - %t thread id
 * - %P process id
 * - %s source filename
 * - %g full source filename
 * - %# source line
 * - %! function name
 * - %^ begin color range
 * - %$ end color range
 * - %% literal percent
 *
 * The underlying formatter may accept additional pattern syntax, but those
 * fields are not part of the RoDAQ logging compatibility contract.
 *
 * fmt is a transitive compile-time dependency of this header because C++17
 * formatting templates are instantiated in caller translation units. spdlog
 * remains a private implementation dependency and no spdlog type is part of
 * the public RoDAQ API vocabulary.
 */
struct Config final {
    std::string name{"rodaq"};
    Level level{Level::Info};

    std::string pattern{"[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%t] [%s:%#] %v"};

    TimeZone time_zone{TimeZone::Local};

    std::vector<SinkConfig> sinks{SinkConfig{ConsoleSinkConfig{}}};

    AsyncConfig async;
    FlushConfig flush;

    /**
     * @brief Number of messages retained by the backtrace ring buffer.
     *
     * Zero disables backtrace buffering. Non-zero values must not exceed 65536.
     * The caller remains responsible for choosing a capacity appropriate for the
     * process memory budget.
     */
    std::size_t backtrace_capacity{0};
};

/**
 * @brief Snapshot of process-local logging health and queue statistics.
 *
 * Counters are cumulative since the most recent successful initialization.
 * Queue fields are meaningful only while asynchronous logging is active.
 */
struct Stats final {
    bool initialized{false};
    bool asynchronous{false};

    std::size_t queue_size{0};
    std::size_t queue_capacity{0};

    std::uint64_t overrun_count{0};
    std::uint64_t discard_count{0};
    std::uint64_t backend_error_count{0};
};

/**
 * @brief Initializes the process-local logger.
 *
 * RoDAQ owns the logger, sinks, asynchronous queue, worker thread, and
 * periodic flush thread. The logger is not registered in the spdlog global
 * registry and does not use the spdlog global thread pool.
 *
 * Calling this function while logging is already initialized returns
 * StatusCode::Conflict.
 *
 * Log calls made before successful initialization use the fallback stderr
 * path with Level::Info as the fallback severity threshold.
 *
 * Config::level sets the initial logger severity threshold.
 *
 * @param config Logger configuration.
 * @return Success or an initialization error.
 */
[[nodiscard]] Result<void> initialize(const Config& config);

/**
 * @brief Shuts down the process-local RoDAQ logging subsystem.
 *
 * shutdown stops accepting new logging calls, stops periodic sink flushing,
 * waits for in-flight logging calls, drains log records remaining in the
 * asynchronous queue, joins the asynchronous worker, and flushes every sink.
 *
 * Log records already discarded by the configured overflow policy are not
 * recoverable during shutdown. shutdown can block while queued records are
 * processed or while a sink performs I/O; no finite upper bound is guaranteed.
 *
 * A successful return means queued records that were not discarded were
 * processed and each sink completed flush(). It does not imply fsync or
 * physical-media durability.
 *
 * If this function returns an error, the logging subsystem is already closed.
 * The error reports a failure observed during shutdown; shutdown is not
 * retryable. A subsequent call is an idempotent no-op. After in-flight logging
 * control operations complete, the fallback severity threshold is reset to
 * Level::Info.
 *
 * Do not call this function from a hard-real-time thread, signal handler, or
 * while holding locks required by logging producers or backend I/O.
 *
 * @return Success or a shutdown/drain/flush error.
 */
[[nodiscard]] Result<void> shutdown();

/**
 * @brief Sets the logger severity threshold.
 *
 * Level::Off disables logger emission to sinks but does not disable an already
 * enabled backtrace ring buffer.
 *
 * Argument validation precedes lifecycle validation. An invalid level returns
 * StatusCode::InvalidArgument. For a valid level, calling this function before
 * initialize() or after shutdown() returns StatusCode::InvalidState.
 *
 * @param level New logger severity threshold.
 * @return Success or an error.
 */
[[nodiscard]] Result<void> set_level(Level level);

/**
 * @brief Enables or resizes the backtrace ring buffer.
 *
 * Argument validation precedes lifecycle validation. A capacity outside
 * [1, 65536] returns StatusCode::InvalidArgument. For a valid capacity, an
 * uninitialized logger returns StatusCode::InvalidState. Allocation failure
 * returns StatusCode::ResourceExhausted and logging-backend failure returns
 * StatusCode::Unavailable.
 *
 * @param capacity Number of messages retained.
 * @return Success or an error.
 */
[[nodiscard]] Result<void> enable_backtrace(std::size_t capacity);

/**
 * @brief Disables backtrace buffering.
 *
 * The logger must be initialized. The operation is idempotent when backtrace
 * buffering is already disabled. An uninitialized logger returns
 * StatusCode::InvalidState and logging-backend failure returns
 * StatusCode::Unavailable.
 *
 * @return Success or an error.
 */
[[nodiscard]] Result<void> disable_backtrace();

/**
 * @brief Requests emission of buffered backtrace messages.
 *
 * The logger must be initialized. An uninitialized logger returns
 * StatusCode::InvalidState and logging-backend failure returns
 * StatusCode::Unavailable.
 *
 * In asynchronous mode, a successful return does not imply that emitted
 * backtrace records have reached their sinks, and the configured overflow
 * policy can still discard queued records.
 *
 * @return Success or an error.
 */
[[nodiscard]] Result<void> dump_backtrace();

/**
 * @brief Requests sink flushing.
 *
 * For synchronous logging, sink flushing is performed before this function
 * returns.
 *
 * For asynchronous logging, an ordered flush request is posted to the
 * asynchronous queue using blocking enqueue semantics. The function returns
 * after the request is queued, not after sink flushing has completed.
 *
 * This operation can block waiting for asynchronous queue capacity.
 *
 * @warning Do not call this function from a hard-real-time thread, signal
 * handler, latency-critical producer path, or while holding locks needed by
 * logging producers.
 *
 * @return Success or an error.
 */
[[nodiscard]] Result<void> request_flush();

/**
 * @brief Returns the active logger or fallback severity threshold.
 *
 * Before initialization and after shutdown this returns Level::Info. While the
 * logger is initialized it returns Config::level or the most recent successful
 * set_level() value.
 */
[[nodiscard]] Level level() noexcept;

/**
 * @brief Returns a best-effort snapshot of logging statistics.
 *
 * This function does not throw. If an asynchronous queue statistic cannot be
 * read, fields already collected remain valid and unavailable queue fields
 * retain their default values.
 */
[[nodiscard]] Stats stats() noexcept;

/**
 * @brief Checks whether a log record requires processing.
 *
 * When backtrace is enabled, this function can return true below the logger
 * severity threshold because the record still needs to be formatted and
 * captured by the backtrace ring buffer.
 *
 * A true result therefore does not guarantee emission to a sink.
 */
[[nodiscard]] bool should_log(Level message_level) noexcept;

/**
 * @brief Emits an already formatted log message.
 *
 * Braces in message are treated literally. Normal application logging should
 * use RODAQ_LOG_* so compile-time level pruning and source capture are
 * preserved.
 *
 * This function never propagates logging-backend exceptions into application code.
 */
void write(Level message_level, SourceLocation source, std::string_view message) noexcept;

/**
 * @brief Formats and writes a log message.
 *
 * The format grammar is provided by the pinned external fmt library.
 * RoDAQ logging intentionally accepts std::string_view so fmt format-string
 * types do not become part of the project API vocabulary.
 *
 * This function performs a defensive should_log() check so direct calls remain
 * filtered. RODAQ_LOG_* performs an earlier check before argument evaluation.
 *
 * Arbitrary runtime text containing braces should be logged with "{}" as the
 * format string, or passed to write() if it is already formatted.
 *
 * Formatting failures never escape into application code.
 */
template <typename... Args>
void log(
    Level message_level, SourceLocation source, std::string_view format, Args&&... args) noexcept {
    if (!should_log(message_level)) {
        return;
    }

    try {
        const auto message = fmt::format(
            fmt::runtime(fmt::string_view{format.data(), format.size()}),
            std::forward<Args>(args)...);

        write(message_level, source, std::string_view{message.data(), message.size()});
    } catch (...) {
        write(Level::Error, source, "log message formatting failed");
    }
}

}  // namespace rodaq::logging

#define RODAQ_LOG_IMPL(level, ...)                                                     \
    do {                                                                               \
        if (::rodaq::logging::should_log(level)) {                                     \
            ::rodaq::logging::log(                                                     \
                level, ::rodaq::logging::SourceLocation{__FILE__, __LINE__, __func__}, \
                __VA_ARGS__);                                                          \
        }                                                                              \
    } while (false)

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_TRACE
#define RODAQ_LOG_TRACE(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Trace, __VA_ARGS__)
#else
#define RODAQ_LOG_TRACE(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_DEBUG
#define RODAQ_LOG_DEBUG(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Debug, __VA_ARGS__)
#else
#define RODAQ_LOG_DEBUG(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_INFO
#define RODAQ_LOG_INFO(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Info, __VA_ARGS__)
#else
#define RODAQ_LOG_INFO(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_WARNING
#define RODAQ_LOG_WARNING(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Warning, __VA_ARGS__)
#else
#define RODAQ_LOG_WARNING(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_ERROR
#define RODAQ_LOG_ERROR(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Error, __VA_ARGS__)
#else
#define RODAQ_LOG_ERROR(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_CRITICAL
#define RODAQ_LOG_CRITICAL(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Critical, __VA_ARGS__)
#else
#define RODAQ_LOG_CRITICAL(...) ((void)0)
#endif
