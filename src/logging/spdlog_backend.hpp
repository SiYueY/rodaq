#ifndef RODAQ_LOGGING_SPDLOG_BACKEND_HPP_
#define RODAQ_LOGGING_SPDLOG_BACKEND_HPP_

#include "logging/logging_context.hpp"

#include <memory>

namespace rodaq {

class SpdlogBackend final : public LoggingBackend {
public:
    SpdlogBackend();
    ~SpdlogBackend() override;

    Result<void> write(const LogRecord& record) override;
    Result<void> flush() override;
    Result<void> shutdown() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq

#endif  // RODAQ_LOGGING_SPDLOG_BACKEND_HPP_
