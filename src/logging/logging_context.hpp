#ifndef RODAQ_LOGGING_CONTEXT_HPP_
#define RODAQ_LOGGING_CONTEXT_HPP_

#include <rodaq/result.hpp>
#include <rodaq/daq_config.hpp>
#include <rodaq/daq_stats.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rodaq {

struct LogRecord {
    LogLevel level = LogLevel::Off;
    std::string category;
    std::string message;
    std::uint64_t sequence = 0;
};

class LoggingBackend {
public:
    virtual ~LoggingBackend() = default;
    virtual Result<void> write(const LogRecord& record) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> shutdown() = 0;
};

class LoggingContext final {
public:
    static Result<std::unique_ptr<LoggingContext>> create(
        LoggingConfig config, std::shared_ptr<LoggingBackend> backend);

    LoggingContext(const LoggingContext&) = delete;
    LoggingContext& operator=(const LoggingContext&) = delete;
    ~LoggingContext();

    void log(LogLevel level, std::string_view category, std::string_view message) noexcept;
    bool flush(std::chrono::steady_clock::time_point deadline) noexcept;
    bool shutdown(std::chrono::steady_clock::time_point deadline) noexcept;
    void quarantine_worker(std::string reason) noexcept;
    LoggingStats stats() const noexcept;

private:
    struct State;
    struct RecordingSink;

    LoggingContext(LoggingConfig config, std::shared_ptr<LoggingBackend> backend);
    static void worker_loop(std::shared_ptr<State> state) noexcept;

    LoggingConfig config_;
    std::shared_ptr<State> state_;
    std::thread worker_;
};

}  // namespace rodaq

#endif  // RODAQ_LOGGING_CONTEXT_HPP_
