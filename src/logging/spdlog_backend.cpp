#include "logging/spdlog_backend.hpp"

#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

spdlog::pattern_time_type to_spdlog_time_zone(TimeZone time_zone) noexcept {
    return time_zone == TimeZone::Utc ? spdlog::pattern_time_type::utc
                                      : spdlog::pattern_time_type::local;
}

Status logging_error(
    StatusCode status_code, std::string_view description, std::string_view detail = {}) {
    std::string message(description);
    if (!detail.empty()) {
        message.append(": ");
        message.append(detail.data(), detail.size());
    }
    return Status(status_code, "logging", 0, std::move(message));
}

Result<void> validate_pattern(std::string_view pattern, TimeZone time_zone) {
    if (pattern.empty()) {
        return logging_error(StatusCode::InvalidArgument, "logging pattern must not be empty");
    }
    try {
        const spdlog::pattern_formatter formatter{
            std::string(pattern), to_spdlog_time_zone(time_zone)};
        static_cast<void>(formatter);
        return {};
    } catch (const std::exception& error) {
        return logging_error(StatusCode::InvalidArgument, "invalid logging pattern", error.what());
    } catch (...) {
        return logging_error(StatusCode::InvalidArgument, "invalid logging pattern");
    }
}

std::string normalized_path(std::string_view value) {
    const std::filesystem::path input{std::string(value)};
    std::error_code error;
    auto absolute = std::filesystem::absolute(input, error);
    if (error) {
        absolute = input;
    }
    return absolute.lexically_normal().generic_string();
}

Result<void> validate_sink_options(const SinkOptions& options, const LoggingConfig& config) {
    if (!options.pattern.empty()) {
        return validate_pattern(options.pattern, config.time_zone);
    }
    return {};
}

void configure_sink(
    const spdlog::sink_ptr& sink, const SinkOptions& options, const LoggingConfig& config) {
    sink->set_level(to_spdlog_level(options.level));
    const std::string& pattern = options.pattern.empty() ? config.pattern : options.pattern;
    sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(
        pattern, to_spdlog_time_zone(config.time_zone)));
}

template <typename SinkConfigType, typename Factory>
Result<spdlog::sink_ptr> make_sink(
    const SinkConfigType& sink_config, const LoggingConfig& config, Factory&& factory) {
    auto valid = validate_sink_options(sink_config.options, config);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    try {
        auto sink = factory();
        configure_sink(sink, sink_config.options, config);
        return sink;
    } catch (const std::bad_alloc&) {
        return logging_error(StatusCode::ResourceExhausted, "failed to allocate logging sink");
    } catch (const spdlog::spdlog_ex& error) {
        return logging_error(
            StatusCode::StorageError, "failed to create logging sink", error.what());
    } catch (const std::exception& error) {
        return logging_error(
            StatusCode::StorageError, "unexpected logging sink failure", error.what());
    } catch (...) {
        return logging_error(StatusCode::StorageError, "unknown logging sink failure");
    }
}

}  // namespace

struct SpdlogBackend::Impl {
    explicit Impl(const LoggingConfig& config)
    : logger(std::make_shared<spdlog::logger>(config.name)) {
        logger->set_level(to_spdlog_level(config.level));
        logger->flush_on(to_spdlog_level(config.flush.level));
    }

    std::shared_ptr<spdlog::logger> logger;
    std::mutex mutex;
};

SpdlogBackend::SpdlogBackend(const LoggingConfig& config) : impl_(std::make_unique<Impl>(config)) {}

Result<std::shared_ptr<SpdlogBackend>> SpdlogBackend::create(const LoggingConfig& config) {
    try {
        auto backend = std::shared_ptr<SpdlogBackend>(new SpdlogBackend(config));
        std::set<std::string> file_paths;
        bool stdout_used = false;
        bool stderr_used = false;
        for (const SinkConfig& sink_config : config.sinks) {
            Result<spdlog::sink_ptr> sink(spdlog::sink_ptr{});
            if (const auto* console = std::get_if<ConsoleSinkConfig>(&sink_config)) {
                bool* used = console->stream == ConsoleStream::Stdout ? &stdout_used : &stderr_used;
                if (*used) {
                    return logging_error(StatusCode::Conflict, "duplicate logging console stream");
                }
                *used = true;
                sink = make_sink(*console, config, [console]() -> spdlog::sink_ptr {
                    if (console->color) {
                        return console->stream == ConsoleStream::Stdout
                                   ? spdlog::sink_ptr(
                                         std::make_shared<spdlog::sinks::stdout_color_sink_mt>())
                                   : spdlog::sink_ptr(
                                         std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
                    }
                    return console->stream == ConsoleStream::Stdout
                               ? spdlog::sink_ptr(std::make_shared<spdlog::sinks::stdout_sink_mt>())
                               : spdlog::sink_ptr(
                                     std::make_shared<spdlog::sinks::stderr_sink_mt>());
                });
            } else if (const auto* file = std::get_if<FileSinkConfig>(&sink_config)) {
                if (!file_paths.insert(normalized_path(file->path)).second) {
                    return logging_error(StatusCode::Conflict, "duplicate logging file path");
                }
                sink = make_sink(*file, config, [file]() -> spdlog::sink_ptr {
                    return std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                        file->path, file->truncate);
                });
            } else if (const auto* rotating = std::get_if<RotatingFileSinkConfig>(&sink_config)) {
                if (!file_paths.insert(normalized_path(rotating->path)).second) {
                    return logging_error(StatusCode::Conflict, "duplicate logging file path");
                }
                sink = make_sink(*rotating, config, [rotating]() -> spdlog::sink_ptr {
                    return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                        rotating->path, rotating->max_size, rotating->max_files,
                        rotating->rotate_on_open);
                });
            } else {
                const auto& daily = std::get<DailyFileSinkConfig>(sink_config);
                if (!file_paths.insert(normalized_path(daily.path)).second) {
                    return logging_error(StatusCode::Conflict, "duplicate logging file path");
                }
                sink = make_sink(daily, config, [&daily]() -> spdlog::sink_ptr {
                    return std::make_shared<spdlog::sinks::daily_file_sink_mt>(
                        daily.path, static_cast<int>(daily.rotation_hour),
                        static_cast<int>(daily.rotation_minute), daily.truncate, daily.max_files);
                });
            }
            if (!sink.ok()) {
                return std::move(sink).status();
            }
            backend->impl_->logger->sinks().push_back(std::move(sink).value());
        }
        return std::shared_ptr<SpdlogBackend>(std::move(backend));
    } catch (const std::bad_alloc&) {
        return logging_error(
            StatusCode::ResourceExhausted, "logging backend initialization exhausted memory");
    } catch (const std::exception& error) {
        return logging_error(
            StatusCode::InternalError, "logging backend initialization failed", error.what());
    } catch (...) {
        return logging_error(
            StatusCode::InternalError, "unknown logging backend initialization failure");
    }
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
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->logger == nullptr) {
            return logging_error(StatusCode::InvalidState, "logging backend is shut down");
        }
        impl_->logger->log(
            spdlog::source_loc{
                record.source_file.c_str(),
                record.source_line,
                record.source_function.c_str(),
            },
            to_spdlog_level(record.level), "[{}] {}", record.category, record.message);
        return {};
    } catch (const std::exception& error) {
        return logging_error(StatusCode::Unavailable, "logging write failed", error.what());
    } catch (...) {
        return logging_error(StatusCode::Unavailable, "unknown logging write failure");
    }
}

Result<void> SpdlogBackend::flush() {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->logger == nullptr) {
            return logging_error(StatusCode::InvalidState, "logging backend is shut down");
        }
        impl_->logger->flush();
        return {};
    } catch (const std::exception& error) {
        return logging_error(StatusCode::Unavailable, "logging flush failed", error.what());
    } catch (...) {
        return logging_error(StatusCode::Unavailable, "unknown logging flush failure");
    }
}

Result<void> SpdlogBackend::shutdown() {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->logger == nullptr) {
            return {};
        }
        impl_->logger->flush();
        impl_->logger.reset();
        return {};
    } catch (const std::exception& error) {
        return logging_error(StatusCode::Unavailable, "logging shutdown failed", error.what());
    } catch (...) {
        return logging_error(StatusCode::Unavailable, "unknown logging shutdown failure");
    }
}

Result<void> SpdlogBackend::set_level(LogLevel level) {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->logger == nullptr) {
            return logging_error(StatusCode::InvalidState, "logging backend is shut down");
        }
        impl_->logger->set_level(to_spdlog_level(level));
        return {};
    } catch (const std::exception& error) {
        return logging_error(StatusCode::Unavailable, "failed to set logging level", error.what());
    } catch (...) {
        return logging_error(StatusCode::Unavailable, "unknown logging level failure");
    }
}

Result<void> SpdlogBackend::enable_backtrace(std::size_t capacity) {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->logger == nullptr) {
            return logging_error(StatusCode::InvalidState, "logging backend is shut down");
        }
        impl_->logger->enable_backtrace(capacity);
        return {};
    } catch (const std::exception& error) {
        return logging_error(
            StatusCode::Unavailable, "failed to enable logging backtrace", error.what());
    } catch (...) {
        return logging_error(StatusCode::Unavailable, "unknown logging backtrace failure");
    }
}

Result<void> SpdlogBackend::disable_backtrace() {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->logger == nullptr) {
            return logging_error(StatusCode::InvalidState, "logging backend is shut down");
        }
        impl_->logger->disable_backtrace();
        return {};
    } catch (const std::exception& error) {
        return logging_error(
            StatusCode::Unavailable, "failed to disable logging backtrace", error.what());
    } catch (...) {
        return logging_error(StatusCode::Unavailable, "unknown logging backtrace failure");
    }
}

Result<void> SpdlogBackend::dump_backtrace() {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->logger == nullptr) {
            return logging_error(StatusCode::InvalidState, "logging backend is shut down");
        }
        impl_->logger->dump_backtrace();
        return {};
    } catch (const std::exception& error) {
        return logging_error(
            StatusCode::Unavailable, "failed to dump logging backtrace", error.what());
    } catch (...) {
        return logging_error(StatusCode::Unavailable, "unknown logging backtrace failure");
    }
}

}  // namespace rodaq
