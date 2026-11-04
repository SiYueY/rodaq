#ifndef RODAQ_RUNTIME_LOGGING_CONTEXT_HPP_
#define RODAQ_RUNTIME_LOGGING_CONTEXT_HPP_

#include <rodaq/result.hpp>
#include <rodaq/runtime/logging.hpp>

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

namespace rodaq::runtime {

struct LogRecord {
    LogLevel level = RODAQ_LOG_OFF;
    std::string category;
    std::string message;
    std::uint64_t sequence = 0;
};

class LoggingBackend {
public:
    virtual ~LoggingBackend() = default;
    virtual Result<void> Write(const LogRecord& record) = 0;
    virtual Result<void> Flush() = 0;
    virtual Result<void> Shutdown() = 0;
};

class LoggingContext final {
public:
    static Result<std::unique_ptr<LoggingContext>> Create(
        LoggingConfig config, std::shared_ptr<LoggingBackend> backend);

    LoggingContext(const LoggingContext&) = delete;
    LoggingContext& operator=(const LoggingContext&) = delete;
    ~LoggingContext();

    void Log(LogLevel level, std::string_view category, std::string_view message) noexcept;
    bool Flush(std::chrono::steady_clock::time_point deadline) noexcept;
    bool Shutdown(std::chrono::steady_clock::time_point deadline) noexcept;
    void QuarantineWorker(std::string reason) noexcept;
    LoggingStats Stats() const noexcept;

private:
    struct State;
    struct RecordingSink;

    LoggingContext(LoggingConfig config, std::shared_ptr<LoggingBackend> backend);
    static void WorkerLoop(std::shared_ptr<State> state) noexcept;

    LoggingConfig config_;
    std::shared_ptr<State> state_;
    std::thread worker_;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_LOGGING_CONTEXT_HPP_
