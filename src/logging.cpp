#include <rodaq/logging.hpp>

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace rodaq::logging {
namespace {

constexpr std::size_t kMaxAsyncQueueSize = 262144;
constexpr Level kFallbackLevel = Level::Info;
constexpr const char* kDefaultPattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%t] [%s:%#] %v";

struct LoggingState final {
    std::vector<spdlog::sink_ptr> sinks;
    std::shared_ptr<spdlog::details::thread_pool> thread_pool;
    std::shared_ptr<spdlog::logger> logger;
};

std::mutex lifecycle_mutex;
std::mutex fallback_mutex;
std::shared_ptr<LoggingState> active_state;
std::atomic<Level> active_level{kFallbackLevel};

bool valid_level(Level value) noexcept {
    return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(Level::Off);
}

spdlog::level::level_enum to_spdlog_level(Level value) noexcept {
    switch (value) {
        case Level::Trace:
            return spdlog::level::trace;
        case Level::Debug:
            return spdlog::level::debug;
        case Level::Info:
            return spdlog::level::info;
        case Level::Warning:
            return spdlog::level::warn;
        case Level::Error:
            return spdlog::level::err;
        case Level::Critical:
            return spdlog::level::critical;
        case Level::Off:
            return spdlog::level::off;
    }
    return spdlog::level::off;
}

const char* level_name(Level value) noexcept {
    switch (value) {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warning:
            return "warning";
        case Level::Error:
            return "error";
        case Level::Critical:
            return "critical";
        case Level::Off:
            return "off";
    }
    return "unknown";
}

void write_fallback(Level msg_level, SourceLocation source, std::string_view message) noexcept {
    auto emit = [&] {
        std::fprintf(stderr, "[rodaq] [%s]", level_name(msg_level));
        if (source.file != nullptr && source.line > 0) {
            std::fprintf(stderr, " [%s:%d]", source.file, source.line);
        }
        std::fputc(' ', stderr);
        if (!message.empty()) {
            std::fwrite(message.data(), 1, message.size(), stderr);
        }
        std::fputc('\n', stderr);
        std::fflush(stderr);
    };
    try {
        std::lock_guard<std::mutex> lock(fallback_mutex);
        emit();
    } catch (...) {
        emit();
    }
}

}  // namespace

Result<void> initialize(const Config& config) {
    if (config.name.empty() || !valid_level(config.level)) {
        return Status(StatusCode::InvalidArgument, "Invalid logging name or level");
    }
    if (!config.console && config.rotating_file_path.empty()) {
        return Status(StatusCode::InvalidArgument, "At least one logging sink is required");
    }
    if (!config.rotating_file_path.empty() &&
        (config.rotating_max_bytes == 0 || config.rotating_max_files == 0)) {
        return Status(StatusCode::InvalidArgument, "Invalid rotating file sink parameters");
    }
    if (config.async &&
        (config.async_queue_size == 0 || config.async_queue_size > kMaxAsyncQueueSize)) {
        return Status(StatusCode::InvalidArgument, "Invalid async logging queue_size");
    }

    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    if (std::atomic_load_explicit(&active_state, std::memory_order_acquire) != nullptr) {
        return Status(StatusCode::Conflict, "Logging is already initialized");
    }

    try {
        auto state = std::make_shared<LoggingState>();
        if (config.console) {
            spdlog::sink_ptr sink;
            if (config.console_stderr) {
                sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
            } else {
                sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            }
            sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(kDefaultPattern));
            state->sinks.push_back(std::move(sink));
        }
        if (!config.rotating_file_path.empty()) {
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.rotating_file_path, config.rotating_max_bytes, config.rotating_max_files);
            sink->set_formatter(std::make_unique<spdlog::pattern_formatter>(kDefaultPattern));
            state->sinks.push_back(std::move(sink));
        }
        if (config.async) {
            state->thread_pool =
                std::make_shared<spdlog::details::thread_pool>(config.async_queue_size, 1);
            state->logger = std::make_shared<spdlog::async_logger>(
                config.name, state->sinks.begin(), state->sinks.end(), state->thread_pool,
                spdlog::async_overflow_policy::overrun_oldest);
        } else {
            state->logger = std::make_shared<spdlog::logger>(
                config.name, state->sinks.begin(), state->sinks.end());
        }
        state->logger->set_level(to_spdlog_level(config.level));
        state->logger->flush_on(spdlog::level::err);
        state->logger->set_error_handler(
            [](const std::string& msg) { write_fallback(Level::Error, {}, msg); });

        active_level.store(config.level, std::memory_order_release);
        std::atomic_store_explicit(&active_state, std::move(state), std::memory_order_release);
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, "Failed to allocate logging state");
    } catch (const std::exception& error) {
        return Status(StatusCode::Unavailable, error.what());
    } catch (...) {
        return Status(StatusCode::Internal, "Unknown logging initialization failure");
    }
}

Result<void> shutdown() {
    std::shared_ptr<LoggingState> state;
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        state = std::atomic_exchange_explicit(
            &active_state, std::shared_ptr<LoggingState>{}, std::memory_order_acq_rel);
        active_level.store(kFallbackLevel, std::memory_order_release);
    }
    if (!state) {
        return {};
    }
    try {
        if (state->logger) {
            state->logger->flush();
        }
        // Release logger first, then thread_pool (drains queued items and joins worker),
        // then flush every sink explicitly.
        state->logger.reset();
        state->thread_pool.reset();
        for (const auto& sink : state->sinks) {
            if (sink) {
                sink->flush();
            }
        }
        state->sinks.clear();
        return {};
    } catch (const std::exception& error) {
        return Status(StatusCode::Unavailable, error.what());
    } catch (...) {
        return Status(StatusCode::Internal, "Logging shutdown failed");
    }
}

Result<void> set_level(Level new_level) {
    if (!valid_level(new_level)) {
        return Status(StatusCode::InvalidArgument, "Invalid logging level");
    }
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    auto state = std::atomic_load_explicit(&active_state, std::memory_order_acquire);
    if (!state || !state->logger) {
        return Status(StatusCode::InvalidState, "Logging is not initialized");
    }
    state->logger->set_level(to_spdlog_level(new_level));
    active_level.store(new_level, std::memory_order_release);
    return {};
}

Level level() noexcept { return active_level.load(std::memory_order_acquire); }

Result<void> flush() {
    auto state = std::atomic_load_explicit(&active_state, std::memory_order_acquire);
    if (!state || !state->logger) {
        return Status(StatusCode::InvalidState, "Logging is not initialized");
    }
    try {
        state->logger->flush();
        return {};
    } catch (const std::exception& error) {
        return Status(StatusCode::Unavailable, error.what());
    } catch (...) {
        return Status(StatusCode::Internal, "Logging flush failed");
    }
}

bool should_log(Level message_level) noexcept {
    const auto threshold = active_level.load(std::memory_order_acquire);
    if (!valid_level(message_level) || !valid_level(threshold) || message_level == Level::Off ||
        threshold == Level::Off) {
        return false;
    }
    return static_cast<std::uint8_t>(message_level) >= static_cast<std::uint8_t>(threshold);
}

void write(Level message_level, SourceLocation source, std::string_view message) noexcept {
    if (!should_log(message_level)) {
        return;
    }
    auto state = std::atomic_load_explicit(&active_state, std::memory_order_acquire);
    if (!state || !state->logger) {
        write_fallback(message_level, source, message);
        return;
    }
    try {
        const spdlog::source_loc loc{source.file, source.line, source.function};
        state->logger->log(
            loc, to_spdlog_level(message_level),
            spdlog::string_view_t{message.data(), message.size()});
    } catch (...) {
        write_fallback(message_level, source, message);
    }
}

}  // namespace rodaq::logging
