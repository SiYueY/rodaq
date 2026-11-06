#include "logging/spdlog_backend.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace rodaq {
namespace {

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return spdlog::level::trace;
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
        case LogLevel::Critical:
            return spdlog::level::critical;
        case LogLevel::Off:
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

Result<void> SpdlogBackend::write(const LogRecord& record) {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return rodaq::Status(StatusCode::InvalidState, "logging backend is shut down");
    }
    try {
        impl_->logger->log(
            to_spdlog_level(record.level), "[{}] {}", record.category, record.message);
        return {};
    } catch (const std::exception& error) {
        return rodaq::Status(StatusCode::StorageError, "spdlog", 0, error.what());
    } catch (...) {
        return rodaq::Status(
            StatusCode::StorageError, "spdlog", 0, "unknown logging backend failure");
    }
}

Result<void> SpdlogBackend::flush() {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return rodaq::Status(StatusCode::InvalidState, "logging backend is shut down");
    }
    try {
        impl_->logger->flush();
        return {};
    } catch (const std::exception& error) {
        return rodaq::Status(StatusCode::StorageError, "spdlog", 0, error.what());
    } catch (...) {
        return rodaq::Status(
            StatusCode::StorageError, "spdlog", 0, "unknown logging flush failure");
    }
}

Result<void> SpdlogBackend::shutdown() {
    if (impl_ == nullptr || impl_->logger == nullptr) {
        return {};
    }
    try {
        impl_->logger->flush();
        impl_->logger.reset();
        return {};
    } catch (const std::exception& error) {
        return rodaq::Status(StatusCode::StorageError, "spdlog", 0, error.what());
    } catch (...) {
        return rodaq::Status(
            StatusCode::StorageError, "spdlog", 0, "unknown logging shutdown failure");
    }
}

}  // namespace rodaq
