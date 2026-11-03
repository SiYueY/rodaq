#include "rodaq/runtime/logger.hpp"

#include <string_view>

namespace rodaq {

void Logger::Log(
    LogLevel level, std::string_view category, std::string_view message) const noexcept {
    if (sink_ == nullptr) {
        return;
    }
    try {
        sink_->Write(level, category, message);
    } catch (...) {
    }
}

}  // namespace rodaq
