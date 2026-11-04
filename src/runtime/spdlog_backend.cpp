#include "runtime/spdlog_backend.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace rodaq::runtime {
namespace {

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
        case RODAQ_LOG_TRACE:
            return spdlog::level::trace;
        case RODAQ_LOG_DEBUG:
            return spdlog::level::debug;
        case RODAQ_LOG_INFO:
            return spdlog::level::info;
        case RODAQ_LOG_WARN:
            return spdlog::level::warn;
        case RODAQ_LOG_ERROR:
            return spdlog::level::err;
        case RODAQ_LOG_CRITICAL:
            return spdlog::level::critical;
        case RODAQ_LOG_OFF:
        default:
            return spdlog::level::off;
    }
}

}  // namespace

struct SpdlogBackend::Impl {
    std::shared_ptr<spdlog::logger> logger;
};

SpdlogBackend::SpdlogBackend() : impl_(std::make_unique<Impl>()) {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    impl_->logger = std::make_shared<spdlog::logger>("rodaq", std::move(sink));
    impl_->logger->set_level(spdlog::level::trace);
    impl_->logger->set_pattern("%Y-%m-%dT%H:%M:%S.%e%z [%l] [%n] %v");
}

SpdlogBackend::~SpdlogBackend() {
    if (impl_ != nullptr && impl_->logger != nullptr) {
        try {
            impl_->logger->flush();
        } catch (...) {
        }
    }
}

Result<void> SpdlogBackend::Write(const LogRecord& record) {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "logging backend is shut down"));
    }
    try {
        impl_->logger->log(
            to_spdlog_level(record.level), "[{}] {}", record.category, record.message);
        return {};
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(Error(ErrorCode::StorageError, "spdlog", 0, error.what()));
    } catch (...) {
        return nonstd::make_unexpected(
            Error(ErrorCode::StorageError, "spdlog", 0, "unknown logging backend failure"));
    }
}

Result<void> SpdlogBackend::Flush() {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "logging backend is shut down"));
    }
    try {
        impl_->logger->flush();
        return {};
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(Error(ErrorCode::StorageError, "spdlog", 0, error.what()));
    } catch (...) {
        return nonstd::make_unexpected(
            Error(ErrorCode::StorageError, "spdlog", 0, "unknown logging flush failure"));
    }
}

Result<void> SpdlogBackend::Shutdown() {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return {};
    }
    try {
        impl_->logger->flush();
        impl_->logger.reset();
        return {};
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(Error(ErrorCode::StorageError, "spdlog", 0, error.what()));
    } catch (...) {
        return nonstd::make_unexpected(
            Error(ErrorCode::StorageError, "spdlog", 0, "unknown logging shutdown failure"));
    }
}

}  // namespace rodaq::runtime
