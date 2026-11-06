#ifndef RODAQ_LOGGING_LOGGER_HPP_
#define RODAQ_LOGGING_LOGGER_HPP_

#include <rodaq/daq_config.hpp>

#include <string_view>

namespace rodaq {

/// A borrowed logging destination owned by the application.
class LoggerSink {
public:
    LoggerSink() = default;
    LoggerSink(const LoggerSink&) = delete;
    LoggerSink& operator=(const LoggerSink&) = delete;
    virtual ~LoggerSink() = default;

    virtual void write(LogLevel level, std::string_view category, std::string_view message) = 0;
};

/// A no-throw logging facade that does not own the sink or global state.
class Logger final {
public:
    Logger() noexcept = default;
    explicit Logger(LoggerSink& sink) noexcept : sink_(&sink) {}

    Logger(const Logger&) = default;
    Logger& operator=(const Logger&) = default;

    bool valid() const noexcept { return sink_ != nullptr; }

    void log(LogLevel level, std::string_view category, std::string_view message) const noexcept;

private:
    LoggerSink* sink_ = nullptr;
};

}  // namespace rodaq

#endif  // RODAQ_LOGGING_LOGGER_HPP_
