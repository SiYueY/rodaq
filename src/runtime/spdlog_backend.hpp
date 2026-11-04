#ifndef RODAQ_RUNTIME_SPDLOG_BACKEND_HPP_
#define RODAQ_RUNTIME_SPDLOG_BACKEND_HPP_

#include "runtime/logging_context.hpp"

#include <memory>

namespace rodaq::runtime {

class SpdlogBackend final : public LoggingBackend {
public:
    SpdlogBackend();
    ~SpdlogBackend() override;

    Result<void> Write(const LogRecord& record) override;
    Result<void> Flush() override;
    Result<void> Shutdown() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_SPDLOG_BACKEND_HPP_
