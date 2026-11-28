#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rodaq/result.hpp>

#if FMT_VERSION != 110200
#error "RoDAQ logging requires fmt 11.2.0 exactly"
#endif

#define RODAQ_LOG_LEVEL_TRACE 0
#define RODAQ_LOG_LEVEL_DEBUG 1
#define RODAQ_LOG_LEVEL_INFO 2
#define RODAQ_LOG_LEVEL_WARNING 3
#define RODAQ_LOG_LEVEL_ERROR 4
#define RODAQ_LOG_LEVEL_CRITICAL 5
#define RODAQ_LOG_LEVEL_OFF 6

#ifndef RODAQ_LOG_ACTIVE_LEVEL
#define RODAQ_LOG_ACTIVE_LEVEL RODAQ_LOG_LEVEL_INFO
#endif

#if RODAQ_LOG_ACTIVE_LEVEL < RODAQ_LOG_LEVEL_TRACE || RODAQ_LOG_ACTIVE_LEVEL > RODAQ_LOG_LEVEL_OFF
#error "RODAQ_LOG_ACTIVE_LEVEL is invalid"
#endif

namespace rodaq::logging {

enum class Level : std::uint8_t {
    Trace = RODAQ_LOG_LEVEL_TRACE,
    Debug = RODAQ_LOG_LEVEL_DEBUG,
    Info = RODAQ_LOG_LEVEL_INFO,
    Warning = RODAQ_LOG_LEVEL_WARNING,
    Error = RODAQ_LOG_LEVEL_ERROR,
    Critical = RODAQ_LOG_LEVEL_CRITICAL,
    Off = RODAQ_LOG_LEVEL_OFF,
};

struct SourceLocation final {
    const char* file{nullptr};
    int line{0};
    const char* function{nullptr};
};

/** Thin spdlog facade configuration: console + rotating file + bounded async queue. */
struct Config final {
    std::string name{"rodaq"};
    Level level{Level::Info};
    bool console{true};
    bool console_stderr{false};
    std::string rotating_file_path;
    std::size_t rotating_max_bytes{50U * 1024U * 1024U};
    std::size_t rotating_max_files{5};
    bool async{true};
    std::size_t async_queue_size{8192};
};

[[nodiscard]] Result<void> initialize(const Config& config);
[[nodiscard]] Result<void> shutdown();
[[nodiscard]] Result<void> set_level(Level level);
[[nodiscard]] Level level() noexcept;
[[nodiscard]] Result<void> flush();
[[nodiscard]] bool should_log(Level message_level) noexcept;

void write(Level message_level, SourceLocation source, std::string_view message) noexcept;

template <typename... Args>
void log(
    Level message_level, SourceLocation source, std::string_view format, Args&&... args) noexcept {
    if (!should_log(message_level)) {
        return;
    }
    try {
        const auto message = fmt::format(
            fmt::runtime(fmt::string_view{format.data(), format.size()}),
            std::forward<Args>(args)...);
        write(message_level, source, std::string_view{message.data(), message.size()});
    } catch (...) {
        write(Level::Error, source, "log message formatting failed");
    }
}

}  // namespace rodaq::logging

#define RODAQ_LOG_IMPL(level, ...)                                                     \
    do {                                                                               \
        if (::rodaq::logging::should_log(level)) {                                     \
            ::rodaq::logging::log(                                                     \
                level, ::rodaq::logging::SourceLocation{__FILE__, __LINE__, __func__}, \
                __VA_ARGS__);                                                          \
        }                                                                              \
    } while (false)

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_TRACE
#define RODAQ_LOG_TRACE(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Trace, __VA_ARGS__)
#else
#define RODAQ_LOG_TRACE(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_DEBUG
#define RODAQ_LOG_DEBUG(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Debug, __VA_ARGS__)
#else
#define RODAQ_LOG_DEBUG(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_INFO
#define RODAQ_LOG_INFO(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Info, __VA_ARGS__)
#else
#define RODAQ_LOG_INFO(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_WARNING
#define RODAQ_LOG_WARNING(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Warning, __VA_ARGS__)
#else
#define RODAQ_LOG_WARNING(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_ERROR
#define RODAQ_LOG_ERROR(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Error, __VA_ARGS__)
#else
#define RODAQ_LOG_ERROR(...) ((void)0)
#endif

#if RODAQ_LOG_ACTIVE_LEVEL <= RODAQ_LOG_LEVEL_CRITICAL
#define RODAQ_LOG_CRITICAL(...) RODAQ_LOG_IMPL(::rodaq::logging::Level::Critical, __VA_ARGS__)
#else
#define RODAQ_LOG_CRITICAL(...) ((void)0)
#endif
