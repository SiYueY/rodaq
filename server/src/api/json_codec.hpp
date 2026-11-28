#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rodaq/task/types.hpp>

namespace rodaq::server {

struct JsonParseLimits {
    std::size_t max_bytes{4194304};
    std::size_t max_depth{32};
};

struct JsonParseError {
    std::string code;
    std::string path;
    std::string message;
};

struct JsonParseResult {
    bool ok{false};
    nlohmann::json value;
    JsonParseError error;
};

// Bounded nlohmann::json DOM parse (checks byte size and nesting depth).
JsonParseResult parse_bounded_json(std::string_view input, const JsonParseLimits& limits = {});

std::optional<std::uint64_t> parse_u64_decimal(std::string_view text) noexcept;
std::string format_u64_decimal(std::uint64_t value);

std::string format_rfc3339_utc(WallTime tp);
std::optional<WallTime> parse_rfc3339_utc(std::string_view text) noexcept;

// Parses duration query filter like "10s", "500ms", "1000us", "5000ns", or plain milliseconds.
std::optional<std::chrono::nanoseconds> parse_duration_query(std::string_view text) noexcept;

bool is_visible_ascii(std::string_view text, std::size_t min_len, std::size_t max_len) noexcept;

}  // namespace rodaq::server
