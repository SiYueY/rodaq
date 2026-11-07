#ifndef RODAQ_LOGGING_SPDLOG_BACKEND_HPP_
#define RODAQ_LOGGING_SPDLOG_BACKEND_HPP_

#include "logging/logging_context.hpp"

#include <rodaq/logging_config.hpp>

#include <memory>

namespace rodaq {

class SpdlogBackend final : public LoggingBackend {
public:
    static Result<std::shared_ptr<SpdlogBackend>> create(const LoggingConfig& config);

    ~SpdlogBackend() override;

    Result<void> write(const LogRecord& record) override;
    Result<void> flush() override;
    Result<void> shutdown() override;
    Result<void> set_level(LogLevel level) override;
    Result<void> enable_backtrace(std::size_t capacity) override;
    Result<void> disable_backtrace() override;
    Result<void> dump_backtrace() override;

private:
    explicit SpdlogBackend(const LoggingConfig& config);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq

#endif  // RODAQ_LOGGING_SPDLOG_BACKEND_HPP_
