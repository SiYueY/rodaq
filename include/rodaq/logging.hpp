#ifndef RODAQ_LOGGING_HPP_
#define RODAQ_LOGGING_HPP_

#include <rodaq/export.hpp>
#include <rodaq/logging_config.hpp>
#include <rodaq/result.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/version.h>

#if SPDLOG_VERSION != SPDLOG_TO_VERSION(1, 17, 0)
#error "RoDAQ logging requires spdlog 1.17.0"
#endif

namespace rodaq {

class LoggerState;
class LoggingContext;

/// Optional diagnostic source metadata. Non-null strings must be
/// null-terminated and are copied before write() returns.
struct SourceLocation {
    const char* file = nullptr;
    int line = 0;
    const char* function = nullptr;
};

/// Thread-safe, activation-scoped logging handle. The category is copied at
/// creation. Logging calls are no-ops after the owning Activation stops.
class RODAQ_EXPORT [[nodiscard]] Logger final {
public:
    Logger() noexcept = default;
    Logger(const Logger&) = default;
    Logger(Logger&&) noexcept = default;
    Logger& operator=(const Logger&) = default;
    Logger& operator=(Logger&&) noexcept = default;
    ~Logger() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    Result<void> set_level(LogLevel level);

    [[nodiscard]] bool should_log(LogLevel level) noexcept;
    void write(LogLevel level, SourceLocation source, std::string_view message) noexcept;
    LoggingStats stats() const noexcept;

    Result<void> request_flush();
    Result<void> enable_backtrace(std::size_t capacity);
    Result<void> disable_backtrace();
    Result<void> dump_backtrace();

    template <typename... Args>
    void log(
        LogLevel level, SourceLocation source, std::string_view format, Args&&... args) noexcept {
        if (!should_log(level)) {
            return;
        }
        try {
            const spdlog::string_view_t backend_format{format.data(), format.size()};
            const auto message = spdlog::fmt_lib::format(
                SPDLOG_FMT_RUNTIME(backend_format), std::forward<Args>(args)...);
            write(level, source, std::string_view{message.data(), message.size()});
        } catch (...) {
            write(LogLevel::Error, source, "log message formatting failed");
        }
    }

private:
    friend class LoggingContext;

    Logger(std::weak_ptr<LoggerState> state, std::string category);

    std::weak_ptr<LoggerState> state_;
    std::string category_;
};

}  // namespace rodaq

#define RODAQ_LOG_LEVEL_TRACE 0
#define RODAQ_LOG_LEVEL_DEBUG 1
#define RODAQ_LOG_LEVEL_INFO 2
#define RODAQ_LOG_LEVEL_WARN 3
#define RODAQ_LOG_LEVEL_ERROR 4
#define RODAQ_LOG_LEVEL_CRITICAL 5
#define RODAQ_LOG_LEVEL_OFF 6

#ifndef RODAQ_LOG_ACTIVE_LEVEL
#define RODAQ_LOG_ACTIVE_LEVEL RODAQ_LOG_LEVEL_INFO
#endif

#if RODAQ_LOG_ACTIVE_LEVEL < RODAQ_LOG_LEVEL_TRACE || RODAQ_LOG_ACTIVE_LEVEL > RODAQ_LOG_LEVEL_OFF
#error "RODAQ_LOG_ACTIVE_LEVEL is invalid"
#endif

#define RODAQ_LOG_IMPL(logger_expression, level_value, ...)                                       \
    do {                                                                                          \
        auto&& rodaq_logger = (logger_expression);                                                \
        if (rodaq_logger.should_log(level_value)) {                                               \
            rodaq_logger.log(                                                                     \
                level_value, ::rodaq::SourceLocation{__FILE__, __LINE__, __func__}, __VA_ARGS__); \
        }                                                                                         \
    } while (false)

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_TRACE
#define RODAQ_LOG_TRACE(logger, ...) RODAQ_LOG_IMPL(logger, ::rodaq::LogLevel::Trace, __VA_ARGS__)
#else
#define RODAQ_LOG_TRACE(logger, ...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_DEBUG
#define RODAQ_LOG_DEBUG(logger, ...) RODAQ_LOG_IMPL(logger, ::rodaq::LogLevel::Debug, __VA_ARGS__)
#else
#define RODAQ_LOG_DEBUG(logger, ...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_INFO
#define RODAQ_LOG_INFO(logger, ...) RODAQ_LOG_IMPL(logger, ::rodaq::LogLevel::Info, __VA_ARGS__)
#else
#define RODAQ_LOG_INFO(logger, ...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_WARN
#define RODAQ_LOG_WARN(logger, ...) RODAQ_LOG_IMPL(logger, ::rodaq::LogLevel::Warn, __VA_ARGS__)
#else
#define RODAQ_LOG_WARN(logger, ...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_ERROR
#define RODAQ_LOG_ERROR(logger, ...) RODAQ_LOG_IMPL(logger, ::rodaq::LogLevel::Error, __VA_ARGS__)
#else
#define RODAQ_LOG_ERROR(logger, ...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_CRITICAL
#define RODAQ_LOG_CRITICAL(logger, ...) \
    RODAQ_LOG_IMPL(logger, ::rodaq::LogLevel::Critical, __VA_ARGS__)
#else
#define RODAQ_LOG_CRITICAL(logger, ...) ((void)0)
#endif

#endif  // RODAQ_LOGGING_HPP_
