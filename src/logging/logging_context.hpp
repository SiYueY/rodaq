#ifndef RODAQ_LOGGING_CONTEXT_HPP_
#define RODAQ_LOGGING_CONTEXT_HPP_

#include <rodaq/logging.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace rodaq {

struct LogRecord {
    LogLevel level = LogLevel::Off;
    std::string category;
    std::string message;
    std::string source_file;
    std::string source_function;
    int source_line = 0;
    std::uint64_t sequence = 0;
};

class LoggingBackend {
public:
    virtual ~LoggingBackend() = default;

    virtual Result<void> write(const LogRecord& record) = 0;
    virtual Result<void> flush() = 0;
    virtual Result<void> shutdown() = 0;
    virtual Result<void> set_level(LogLevel level) = 0;
    virtual Result<void> enable_backtrace(std::size_t capacity) = 0;
    virtual Result<void> disable_backtrace() = 0;
    virtual Result<void> dump_backtrace() = 0;
};

class LoggerState final {
public:
    struct LogItem {
        LogRecord record;
    };
    struct FlushItem {};
    struct DumpBacktraceItem {};
    using QueueItem = std::variant<LogItem, FlushItem, DumpBacktraceItem>;

    LoggerState(LoggingConfig config, std::shared_ptr<LoggingBackend> backend);

    LoggerState(const LoggerState&) = delete;
    LoggerState& operator=(const LoggerState&) = delete;

    LoggingStats stats() const noexcept;
    void record_backend_failure() noexcept;

private:
    friend class LoggingContext;
    friend class Logger;

    LoggingConfig config;
    std::shared_ptr<LoggingBackend> backend;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<QueueItem> queue;
    std::size_t queued_bytes = 0;
    std::uint64_t next_sequence = 1;
    std::uint64_t last_terminal_sequence = 0;
    LoggingStats stats_value;
    std::atomic<LogLevel> level{LogLevel::Info};
    std::atomic<bool> accepting{true};
    std::atomic<bool> backtrace_enabled{false};
    bool stopping = false;
    bool worker_done = false;
    bool backend_failed = false;
    std::size_t active_writers = 0;
    std::shared_ptr<std::atomic<bool>> worker_done_signal =
        std::make_shared<std::atomic<bool>>(false);
};

class LoggingContext final {
public:
    static Result<std::unique_ptr<LoggingContext>> create(
        LoggingConfig config, std::shared_ptr<LoggingBackend> backend);

    LoggingContext(const LoggingContext&) = delete;
    LoggingContext& operator=(const LoggingContext&) = delete;
    ~LoggingContext();

    Result<Logger> make_logger(std::string_view category);
    void log(
        LogLevel level, std::string_view category, SourceLocation source,
        std::string_view message) noexcept;
    bool flush(std::chrono::steady_clock::time_point deadline) noexcept;
    bool shutdown(std::chrono::steady_clock::time_point deadline) noexcept;
    void quarantine_worker(std::string reason) noexcept;
    LoggingStats stats() const noexcept;

private:
    LoggingContext(LoggingConfig config, std::shared_ptr<LoggingBackend> backend);
    static void worker_loop(std::shared_ptr<LoggerState> state) noexcept;

    LoggingConfig config_;
    std::shared_ptr<LoggerState> state_;
    std::thread worker_;
};

}  // namespace rodaq

#endif  // RODAQ_LOGGING_CONTEXT_HPP_
