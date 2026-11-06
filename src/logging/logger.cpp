#include "logging/logger.hpp"

#include <string_view>

namespace rodaq {

void Logger::log(
    LogLevel level, std::string_view category, std::string_view message) const noexcept {
    if (sink_ == nullptr) {
        return;
    }
    try {
        sink_->write(level, category, message);
    } catch (...) {
    }
}

}  // namespace rodaq
