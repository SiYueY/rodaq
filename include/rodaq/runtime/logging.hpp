#ifndef RODAQ_RUNTIME_LOGGING_HPP_
#define RODAQ_RUNTIME_LOGGING_HPP_

#include <rodaq/runtime/logger.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace rodaq {

struct LoggingConfig {
    LogLevel level = RODAQ_LOG_INFO;
    std::size_t queue_records = 8192;
    std::size_t queue_bytes = 8U * 1024U * 1024U;
    std::chrono::nanoseconds flush_timeout = std::chrono::seconds(2);
    std::chrono::nanoseconds shutdown_timeout = std::chrono::seconds(3);
    bool stderr_sink = true;
};

struct LoggingStats {
    std::uint64_t records_submitted = 0;
    std::uint64_t records_written = 0;
    std::uint64_t records_dropped = 0;
    std::uint64_t bytes_dropped = 0;
    std::uint64_t queue_records_current = 0;
    std::uint64_t queue_records_high_water = 0;
    std::uint64_t queue_bytes_current = 0;
    std::uint64_t queue_bytes_high_water = 0;
    std::uint64_t backend_failures = 0;
    std::uint64_t reentrant_drops = 0;
    std::uint64_t flush_timeouts = 0;
    std::uint64_t shutdown_timeouts = 0;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_LOGGING_HPP_
