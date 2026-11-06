#include "logging/logger.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace rodaq {
namespace {

class RecordingSink final : public LoggerSink {
public:
    void write(LogLevel level, std::string_view category, std::string_view message) override {
        level_ = level;
        category_ = std::string(category);
        message_ = std::string(message);
        ++calls_;
    }

    LogLevel level_ = LogLevel::Off;
    std::string category_;
    std::string message_;
    int calls_ = 0;
};

class ThrowingSink final : public LoggerSink {
public:
    void write(LogLevel, std::string_view, std::string_view) override {
        throw std::runtime_error("sink failure");
    }
};

TEST(LoggerTest, ForwardsLevelCategoryAndMessage) {
    RecordingSink sink;
    const Logger logger(sink);
    logger.log(LogLevel::Warn, "daq.admission", "resource pressure");

    EXPECT_EQ(1, sink.calls_);
    EXPECT_EQ(LogLevel::Warn, sink.level_);
    EXPECT_EQ("daq.admission", sink.category_);
    EXPECT_EQ("resource pressure", sink.message_);
}

TEST(LoggerTest, InvalidAndThrowingSinksDoNotPropagateExceptions) {
    const Logger invalid;
    EXPECT_FALSE(invalid.valid());
    EXPECT_NO_THROW(invalid.log(LogLevel::Error, "daq", "ignored"));

    ThrowingSink sink;
    const Logger logger(sink);
    EXPECT_NO_THROW(logger.log(LogLevel::Error, "daq", "caught"));
}

}  // namespace
}  // namespace rodaq
