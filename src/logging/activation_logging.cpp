#include "activation/activation_internal.hpp"
#include "logging/spdlog_backend.hpp"

namespace rodaq {

void ActivationState::log(
    LogLevel level, std::string_view category, std::string_view message) noexcept {
    if (logging_context_ != nullptr) {
        logging_context_->log(level, category, message);
    }
}

void ActivationState::diagnostic(
    rodaq_diagnostic_severity severity, std::string_view code, std::string_view message) noexcept {
    LogLevel level = LogLevel::Off;
    switch (severity) {
        case RODAQ_DIAGNOSTIC_INFO:
            level = LogLevel::Info;
            break;
        case RODAQ_DIAGNOSTIC_WARNING:
            level = LogLevel::Warn;
            break;
        case RODAQ_DIAGNOSTIC_ERROR:
            level = LogLevel::Error;
            break;
        case RODAQ_DIAGNOSTIC_FATAL:
            level = LogLevel::Critical;
            break;
        default:
            return;
    }
    try {
        const std::string text = std::string(code) + ": " + std::string(message);
        log(level, "rodaq.diagnostic", text);
    } catch (...) {
    }
}

bool ActivationState::shutdown_logging(std::chrono::steady_clock::time_point deadline) noexcept {
    return logging_context_ == nullptr || logging_context_->shutdown(deadline);
}

void ActivationState::quarantine_logging(std::string reason) noexcept {
    if (logging_context_ != nullptr) {
        logging_context_->quarantine_worker(std::move(reason));
    }
}

LoggingStats ActivationState::current_logging_stats() const noexcept {
    return logging_context_ == nullptr ? LoggingStats{} : logging_context_->stats();
}

}  // namespace rodaq
