#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rodaq/result.hpp>
#include <rodaq/task/types.hpp>

namespace rodaq::server {

struct StrictJsonLimits {
    std::size_t max_bytes{4194304};
    std::size_t max_depth{64};
    std::size_t max_string_bytes{1048576};
    std::size_t max_members{4096};
    std::size_t max_nodes{65536};
};

struct JsonParseError {
    std::string code;
    std::string path;
    std::string message;
};

struct StrictJsonResult {
    bool ok{false};
    nlohmann::json value;
    JsonParseError error;
};

StrictJsonResult parse_strict_json(std::string_view input, const StrictJsonLimits& limits = {});

bool is_valid_utf8(std::string_view text) noexcept;

bool is_visible_ascii(std::string_view text, std::size_t min_len, std::size_t max_len) noexcept;

std::optional<std::uint64_t> parse_u64_decimal(std::string_view text) noexcept;

std::string format_rfc3339_utc(WallTime timestamp);

std::optional<WallTime> parse_rfc3339_utc(std::string_view text) noexcept;

nlohmann::json format_duration_ns(std::chrono::nanoseconds duration);

nlohmann::json format_duration_ms(std::chrono::nanoseconds duration);

std::optional<std::chrono::nanoseconds> parse_duration_object(
    const nlohmann::json& object, std::string& error_code, std::string& error_message);

std::optional<std::chrono::nanoseconds> parse_duration_query(std::string_view text) noexcept;

std::string escape_json_pointer(std::string_view token);

}  // namespace rodaq::server
