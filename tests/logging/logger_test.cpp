#include "logging/logging_context.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq {
namespace {

class RecordingBackend final : public LoggingBackend {
public:
    Result<void> write(const LogRecord& record) override {
        std::lock_guard<std::mutex> lock(mutex_);
        records_.push_back(record);
        return {};
    }

    Result<void> flush() override { return {}; }
    Result<void> shutdown() override { return {}; }
    Result<void> set_level(LogLevel) override { return {}; }
    Result<void> enable_backtrace(std::size_t) override { return {}; }
    Result<void> disable_backtrace() override { return {}; }
    Result<void> dump_backtrace() override { return {}; }

    std::vector<LogRecord> records() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<LogRecord> records_;
};

TEST(LoggerTest, FormatsCategoryAndSourceLocation) {
    auto backend = std::make_shared<RecordingBackend>();
    LoggingConfig config;
    config.async.enabled = false;
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());

    auto logger = context.value()->make_logger("rodaq.test");
    ASSERT_TRUE(logger.ok());
    RODAQ_LOG_INFO(logger.value(), "value={}", 42);

    const auto records = backend->records();
    ASSERT_EQ(1U, records.size());
    EXPECT_EQ("rodaq.test", records[0].category);
    EXPECT_EQ("value=42", records[0].message);
    EXPECT_NE(std::string::npos, records[0].source_file.find("logger_test"));
    EXPECT_GT(records[0].source_line, 0);
    EXPECT_FALSE(records[0].source_function.empty());

    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggerTest, AsyncRecordOwnsDynamicSourceLocation) {
    auto backend = std::make_shared<RecordingBackend>();
    auto context = LoggingContext::create(LoggingConfig{}, backend);
    ASSERT_TRUE(context.ok());
    auto logger = context.value()->make_logger("rodaq.test");
    ASSERT_TRUE(logger.ok());

    std::string file = "dynamic_source.cpp";
    std::string function = "dynamic_function";
    logger.value().write(
        LogLevel::Info, SourceLocation{file.c_str(), 73, function.c_str()}, "owned source");
    file.assign("mutated");
    function.assign("mutated");

    ASSERT_TRUE(context.value()->flush(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const auto records = backend->records();
    ASSERT_EQ(1U, records.size());
    EXPECT_EQ("dynamic_source.cpp", records[0].source_file);
    EXPECT_EQ(73, records[0].source_line);
    EXPECT_EQ("dynamic_function", records[0].source_function);

    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggerTest, InvalidLoggerAndControlOperationsAreSafe) {
    Logger logger;
    EXPECT_FALSE(logger.valid());
    EXPECT_EQ(LogLevel::Off, logger.level());
    EXPECT_FALSE(logger.should_log(LogLevel::Error));
    EXPECT_NO_THROW(RODAQ_LOG_ERROR(logger, "ignored {}", 1));
    EXPECT_EQ(StatusCode::InvalidState, logger.set_level(LogLevel::Debug).status().code());
    EXPECT_EQ(StatusCode::InvalidState, logger.request_flush().status().code());
    EXPECT_EQ(StatusCode::InvalidState, logger.enable_backtrace(8).status().code());
    EXPECT_EQ(StatusCode::InvalidState, logger.disable_backtrace().status().code());
    EXPECT_EQ(StatusCode::InvalidState, logger.dump_backtrace().status().code());
}

TEST(LoggerTest, DisabledCompileTimeMacrosDoNotEvaluateArguments) {
    auto backend = std::make_shared<RecordingBackend>();
    LoggingConfig config;
    config.async.enabled = false;
    config.level = LogLevel::Trace;
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());
    auto logger = context.value()->make_logger("rodaq.test");
    ASSERT_TRUE(logger.ok());

    int evaluations = 0;
    const auto evaluate = [&evaluations]() {
        ++evaluations;
        return 7;
    };
    RODAQ_LOG_DEBUG(logger.value(), "disabled {}", evaluate());
    EXPECT_EQ(0, evaluations);

    RODAQ_LOG_INFO(logger.value(), "enabled {}", evaluate());
    EXPECT_EQ(1, evaluations);
    const auto records = backend->records();
    ASSERT_EQ(1U, records.size());
    EXPECT_EQ("enabled 7", records[0].message);

    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

TEST(LoggerTest, CategoryLevelAndStopLifetimeFollowTheContract) {
    auto backend = std::make_shared<RecordingBackend>();
    LoggingConfig config;
    config.level = LogLevel::Info;
    config.async.enabled = false;
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());

    EXPECT_EQ(StatusCode::InvalidArgument, context.value()->make_logger("").status().code());
    auto logger = context.value()->make_logger("rodaq.test");
    ASSERT_TRUE(logger.ok());
    EXPECT_TRUE(logger.value().valid());
    EXPECT_EQ(LogLevel::Info, logger.value().level());
    EXPECT_FALSE(logger.value().should_log(LogLevel::Debug));
    EXPECT_TRUE(logger.value().should_log(LogLevel::Info));
    logger.value().write(LogLevel::Debug, {}, "filtered");
    EXPECT_TRUE(backend->records().empty());

    ASSERT_TRUE(logger.value().set_level(LogLevel::Debug).ok());
    EXPECT_EQ(LogLevel::Debug, logger.value().level());
    EXPECT_TRUE(logger.value().should_log(LogLevel::Debug));
    logger.value().write(LogLevel::Debug, {}, "before stop");

    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    EXPECT_FALSE(logger.value().valid());
    EXPECT_EQ(LogLevel::Off, logger.value().level());
    EXPECT_FALSE(logger.value().should_log(LogLevel::Error));
    logger.value().write(LogLevel::Error, {}, "after stop");
    EXPECT_EQ(StatusCode::InvalidState, logger.value().set_level(LogLevel::Info).status().code());
    EXPECT_EQ(StatusCode::InvalidState, logger.value().request_flush().status().code());
    EXPECT_EQ(StatusCode::InvalidState, logger.value().dump_backtrace().status().code());
    EXPECT_EQ(1U, backend->records().size());
}

TEST(LoggerTest, FormattingFailureIsContained) {
    auto backend = std::make_shared<RecordingBackend>();
    LoggingConfig config;
    config.async.enabled = false;
    auto context = LoggingContext::create(config, backend);
    ASSERT_TRUE(context.ok());
    auto logger = context.value()->make_logger("rodaq.test");
    ASSERT_TRUE(logger.ok());

    EXPECT_NO_THROW(RODAQ_LOG_INFO(logger.value(), "unmatched {"));
    const auto records = backend->records();
    ASSERT_EQ(1U, records.size());
    EXPECT_EQ(LogLevel::Error, records[0].level);
    EXPECT_EQ("log message formatting failed", records[0].message);

    ASSERT_TRUE(
        context.value()->shutdown(std::chrono::steady_clock::now() + std::chrono::seconds(1)));
}

}  // namespace
}  // namespace rodaq
