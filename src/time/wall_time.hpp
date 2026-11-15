#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::time {

using WallTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

// Stable UTC nanosecond representation for persistent control facts. Parsing
// accepts this canonical form only; transport may normalize other RFC3339 input.
Result<std::string> format_wall_time(WallTime value);
Result<WallTime> parse_wall_time(std::string_view text);

}  // namespace rodaq::time
