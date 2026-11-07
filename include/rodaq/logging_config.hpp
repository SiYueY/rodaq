#ifndef RODAQ_LOGGING_CONFIG_HPP_
#define RODAQ_LOGGING_CONFIG_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rodaq {

enum class LogLevel : std::uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
    Off = 6,
};

enum class TimeZone : std::uint8_t {
    Local,
    Utc,
};

enum class OverflowPolicy : std::uint8_t {
    Block,
    OverrunOldest,
    DiscardNew,
};

enum class ConsoleStream : std::uint8_t {
    Stdout,
    Stderr,
};

struct SinkOptions {
    LogLevel level = LogLevel::Trace;
    std::string pattern;
};

struct ConsoleSinkConfig {
    SinkOptions options;
    ConsoleStream stream = ConsoleStream::Stderr;
    bool color = false;
};

struct FileSinkConfig {
    SinkOptions options;
    std::string path;
    bool truncate = false;
};

struct RotatingFileSinkConfig {
    SinkOptions options;
    std::string path;
    std::size_t max_size = 50U * 1024U * 1024U;
    std::size_t max_files = 5;
    bool rotate_on_open = false;
};

struct DailyFileSinkConfig {
    SinkOptions options;
    std::string path;
    std::uint8_t rotation_hour = 0;
    std::uint8_t rotation_minute = 0;
    bool truncate = false;
    std::uint16_t max_files = 0;
};

using SinkConfig =
    std::variant<ConsoleSinkConfig, FileSinkConfig, RotatingFileSinkConfig, DailyFileSinkConfig>;

struct AsyncLoggingConfig {
    bool enabled = true;
    std::size_t queue_records = 8192;
    std::size_t queue_bytes = 8U * 1024U * 1024U;
    OverflowPolicy overflow_policy = OverflowPolicy::DiscardNew;
};

struct LoggingFlushConfig {
    LogLevel level = LogLevel::Error;
    std::chrono::milliseconds interval{0};
};

struct LoggingConfig {
    std::string name = "rodaq";
    LogLevel level = LogLevel::Info;
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e%z] [%l] [%n] [%t] [%s:%#] %v";
    TimeZone time_zone = TimeZone::Local;
    std::vector<SinkConfig> sinks{SinkConfig{ConsoleSinkConfig{}}};
    AsyncLoggingConfig async;
    LoggingFlushConfig flush;
    std::size_t backtrace_capacity = 0;
    std::chrono::nanoseconds flush_timeout = std::chrono::seconds(2);
    std::chrono::nanoseconds shutdown_timeout = std::chrono::seconds(3);
};

struct LoggingStats {
    bool initialized = false;
    bool asynchronous = false;
    bool backtrace_enabled = false;

    std::uint64_t records_submitted = 0;
    std::uint64_t records_written = 0;
    std::uint64_t records_dropped = 0;
    std::uint64_t bytes_dropped = 0;

    std::uint64_t queue_records_capacity = 0;
    std::uint64_t queue_bytes_capacity = 0;
    std::uint64_t queue_records_current = 0;
    std::uint64_t queue_records_high_water = 0;
    std::uint64_t queue_bytes_current = 0;
    std::uint64_t queue_bytes_high_water = 0;

    std::uint64_t overrun_count = 0;
    std::uint64_t discard_count = 0;
    std::uint64_t backend_failures = 0;
    std::uint64_t reentrant_drops = 0;
    std::uint64_t flush_timeouts = 0;
    std::uint64_t shutdown_timeouts = 0;
};

}  // namespace rodaq

#endif  // RODAQ_LOGGING_CONFIG_HPP_
