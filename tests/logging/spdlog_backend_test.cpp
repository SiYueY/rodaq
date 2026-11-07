#include "logging/spdlog_backend.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace rodaq {
namespace {

namespace fs = std::filesystem;

fs::path make_test_directory(std::string_view name) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto path = fs::temp_directory_path() /
                      ("rodaq-logging-" + std::string(name) + "-" + std::to_string(::getpid()) +
                       "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

LogRecord record(
    LogLevel level, std::string category, std::string message,
    SourceLocation source = SourceLocation{"backend_test.cpp", 42, "test"}) {
    LogRecord value;
    value.level = level;
    value.category = std::move(category);
    value.message = std::move(message);
    value.source_file = source.file == nullptr ? "" : source.file;
    value.source_line = source.line;
    value.source_function = source.function == nullptr ? "" : source.function;
    value.sequence = 1;
    return value;
}

LoggingConfig file_config(const fs::path& path) {
    LoggingConfig config;
    config.name = "rodaq.backend.test";
    config.level = LogLevel::Trace;
    config.pattern = "%v";
    config.async.enabled = false;
    FileSinkConfig sink;
    sink.path = path.string();
    sink.options.pattern = "%v";
    config.sinks = {SinkConfig{sink}};
    return config;
}

TEST(SpdlogBackendTest, WritesConsoleAndEveryFileSinkType) {
    const fs::path directory = make_test_directory("sinks");
    const fs::path file_path = directory / "basic.log";
    const fs::path rotating_path = directory / "rotating.log";
    const fs::path daily_base = directory / "daily.log";

    LoggingConfig config;
    config.name = "rodaq.backend.sinks";
    config.level = LogLevel::Trace;
    config.pattern = "%v";
    config.async.enabled = false;
    config.flush.level = LogLevel::Trace;

    ConsoleSinkConfig console;
    console.options.pattern = "%v";
    console.stream = ConsoleStream::Stdout;

    FileSinkConfig file;
    file.path = file_path.string();
    file.options.pattern = "%v";

    RotatingFileSinkConfig rotating;
    rotating.path = rotating_path.string();
    rotating.max_size = 4096;
    rotating.max_files = 2;
    rotating.options.pattern = "%v";

    DailyFileSinkConfig daily;
    daily.path = daily_base.string();
    daily.options.pattern = "%v";
    config.sinks = {SinkConfig{console}, SinkConfig{file}, SinkConfig{rotating}, SinkConfig{daily}};

    auto backend = SpdlogBackend::create(config);
    ASSERT_TRUE(backend.ok());
    ASSERT_TRUE(backend.value()->write(record(LogLevel::Info, "rodaq.sink", "all sinks")).ok());
    ASSERT_TRUE(backend.value()->flush().ok());

    EXPECT_EQ("[rodaq.sink] all sinks\n", read_file(file_path));
    EXPECT_EQ("[rodaq.sink] all sinks\n", read_file(rotating_path));

    bool found_daily = false;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().filename().string().rfind("daily_", 0) == 0) {
            found_daily = true;
            EXPECT_EQ("[rodaq.sink] all sinks\n", read_file(entry.path()));
        }
    }
    EXPECT_TRUE(found_daily);
    ASSERT_TRUE(backend.value()->shutdown().ok());
}

TEST(SpdlogBackendTest, AppliesSinkLevelsAndPatternsIndependently) {
    const fs::path directory = make_test_directory("sink_levels");
    const fs::path error_path = directory / "error.log";
    const fs::path trace_path = directory / "trace.log";

    LoggingConfig config;
    config.name = "rodaq.backend.pattern";
    config.level = LogLevel::Trace;
    config.async.enabled = false;

    FileSinkConfig error_sink;
    error_sink.path = error_path.string();
    error_sink.options.level = LogLevel::Error;
    error_sink.options.pattern = "%v";

    FileSinkConfig trace_sink;
    trace_sink.path = trace_path.string();
    trace_sink.options.level = LogLevel::Trace;
    trace_sink.options.pattern = "%n|%s:%#|%v";
    config.sinks = {SinkConfig{error_sink}, SinkConfig{trace_sink}};

    auto backend = SpdlogBackend::create(config);
    ASSERT_TRUE(backend.ok());
    ASSERT_TRUE(backend.value()->write(record(LogLevel::Info, "rodaq.sink", "visible")).ok());
    ASSERT_TRUE(backend.value()->flush().ok());

    EXPECT_TRUE(read_file(error_path).empty());
    EXPECT_EQ(
        "rodaq.backend.pattern|backend_test.cpp:42|[rodaq.sink] visible\n", read_file(trace_path));
    ASSERT_TRUE(backend.value()->shutdown().ok());
}

TEST(SpdlogBackendTest, RejectsDuplicateConsoleStreamsAndNormalizedFilePaths) {
    const fs::path directory = make_test_directory("duplicates");

    LoggingConfig duplicated_console;
    duplicated_console.name = "rodaq.backend.duplicate_console";
    duplicated_console.sinks = {
        SinkConfig{ConsoleSinkConfig{}},
        SinkConfig{ConsoleSinkConfig{}},
    };
    auto console_result = SpdlogBackend::create(duplicated_console);
    ASSERT_FALSE(console_result.ok());
    EXPECT_EQ(StatusCode::Conflict, console_result.status().code());

    const fs::path path = directory / "same.log";
    FileSinkConfig file;
    file.path = path.string();
    RotatingFileSinkConfig rotating;
    rotating.path = (directory / "." / "same.log").string();

    LoggingConfig duplicated_file;
    duplicated_file.name = "rodaq.backend.duplicate_file";
    duplicated_file.sinks = {SinkConfig{file}, SinkConfig{rotating}};
    auto file_result = SpdlogBackend::create(duplicated_file);
    ASSERT_FALSE(file_result.ok());
    EXPECT_EQ(StatusCode::Conflict, file_result.status().code());
}

TEST(SpdlogBackendTest, ReportsFileInitializationFailuresWithoutThrowing) {
    const fs::path directory = make_test_directory("file_failure");
    LoggingConfig config;
    config.name = "rodaq.backend.file_failure";
    FileSinkConfig sink;
    sink.path = directory.string();
    config.sinks = {SinkConfig{sink}};

    std::optional<Result<std::shared_ptr<SpdlogBackend>>> backend;
    EXPECT_NO_THROW(backend.emplace(SpdlogBackend::create(config)));
    ASSERT_TRUE(backend.has_value());
    ASSERT_FALSE(backend->ok());
    EXPECT_EQ(StatusCode::StorageError, backend->status().code());
}

TEST(SpdlogBackendTest, BacktraceCapturesFilteredRecordsUntilDumped) {
    const fs::path directory = make_test_directory("backtrace");
    const fs::path path = directory / "backtrace.log";
    auto config = file_config(path);
    config.level = LogLevel::Error;

    auto backend = SpdlogBackend::create(config);
    ASSERT_TRUE(backend.ok());
    ASSERT_TRUE(backend.value()->enable_backtrace(8).ok());
    ASSERT_TRUE(
        backend.value()->write(record(LogLevel::Debug, "rodaq.backtrace", "captured")).ok());
    ASSERT_TRUE(backend.value()->flush().ok());
    EXPECT_TRUE(read_file(path).empty());

    ASSERT_TRUE(backend.value()->dump_backtrace().ok());
    ASSERT_TRUE(backend.value()->flush().ok());
    const std::string output = read_file(path);
    EXPECT_NE(std::string::npos, output.find("Backtrace Start"));
    EXPECT_NE(std::string::npos, output.find("[rodaq.backtrace] captured"));
    EXPECT_NE(std::string::npos, output.find("Backtrace End"));

    ASSERT_TRUE(backend.value()->disable_backtrace().ok());
    ASSERT_TRUE(backend.value()->shutdown().ok());
}

TEST(SpdlogBackendTest, SerializesControlOperationsWithBackendShutdown) {
    const fs::path directory = make_test_directory("control_race");
    const fs::path path = directory / "race.log";
    auto backend = SpdlogBackend::create(file_config(path));
    ASSERT_TRUE(backend.ok());

    ASSERT_TRUE(backend.value()->set_level(LogLevel::Warn).ok());
    ASSERT_TRUE(backend.value()->enable_backtrace(4).ok());
    ASSERT_TRUE(backend.value()->disable_backtrace().ok());
    ASSERT_TRUE(backend.value()->shutdown().ok());
    EXPECT_EQ(StatusCode::InvalidState, backend.value()->set_level(LogLevel::Info).status().code());
    EXPECT_EQ(StatusCode::InvalidState, backend.value()->flush().status().code());
}

}  // namespace
}  // namespace rodaq
