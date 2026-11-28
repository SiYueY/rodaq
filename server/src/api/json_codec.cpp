#include "json_codec.hpp"

#include <charconv>
#include <cstdio>
#include <ctime>
#include <limits>

namespace rodaq::server {
namespace {

bool check_depth(const nlohmann::json& value, std::size_t depth, std::size_t max_depth) noexcept {
    if (depth > max_depth) {
        return false;
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            (void)key;
            if (!check_depth(item, depth + 1, max_depth)) {
                return false;
            }
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (!check_depth(item, depth + 1, max_depth)) {
                return false;
            }
        }
    }
    return true;
}

bool parse_fixed_digits(std::string_view s, std::size_t pos, std::size_t len, int& out) noexcept {
    if (pos + len > s.size()) {
        return false;
    }
    int val = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        val = val * 10 + (c - '0');
    }
    out = val;
    return true;
}

std::int64_t days_from_civil(int y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

}  // namespace

JsonParseResult parse_bounded_json(std::string_view input, const JsonParseLimits& limits) {
    JsonParseResult out;
    if (input.size() > limits.max_bytes) {
        out.error = {"too_large", "", "JSON payload exceeds maximum allowed bytes"};
        return out;
    }
    nlohmann::json parsed = nlohmann::json::parse(input.begin(), input.end(), nullptr, false);
    if (parsed.is_discarded()) {
        out.error = {"invalid_format", "", "invalid JSON syntax"};
        return out;
    }
    if (!check_depth(parsed, 0, limits.max_depth)) {
        out.error = {"too_large", "", "JSON nesting depth exceeds limit"};
        return out;
    }
    out.ok = true;
    out.value = std::move(parsed);
    return out;
}

std::optional<std::uint64_t> parse_u64_decimal(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    if (text.size() > 1 && text.front() == '0') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string format_u64_decimal(std::uint64_t value) { return std::to_string(value); }

std::string format_rfc3339_utc(WallTime tp) {
    const auto total_ns = tp.time_since_epoch().count();
    std::int64_t secs = total_ns / 1000000000LL;
    std::int64_t rem_ns = total_ns % 1000000000LL;
    if (rem_ns < 0) {
        rem_ns += 1000000000LL;
        --secs;
    }
    const auto t = static_cast<std::time_t>(secs);
    std::tm utc{};
    gmtime_r(&t, &utc);

    char base[32];
    std::snprintf(
        base, sizeof(base), "%04d-%02d-%02dT%02d:%02d:%02d", utc.tm_year + 1900, utc.tm_mon + 1,
        utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    if (rem_ns == 0) {
        return std::string(base) + "Z";
    }
    char frac[16];
    std::snprintf(frac, sizeof(frac), ".%09lld", static_cast<long long>(rem_ns));
    std::string frac_str(frac);
    while (frac_str.size() > 2 && frac_str.back() == '0') {
        frac_str.pop_back();
    }
    return std::string(base) + frac_str + "Z";
}

std::optional<WallTime> parse_rfc3339_utc(std::string_view text) noexcept {
    if (text.size() < 20 || text.back() != 'Z') {
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_fixed_digits(text, 0, 4, year) || !parse_fixed_digits(text, 5, 2, month) ||
        !parse_fixed_digits(text, 8, 2, day) || !parse_fixed_digits(text, 11, 2, hour) ||
        !parse_fixed_digits(text, 14, 2, minute) || !parse_fixed_digits(text, 17, 2, second)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    std::int64_t frac_ns = 0;
    if (text.size() > 20) {
        if (text[19] != '.') {
            return std::nullopt;
        }
        const auto frac = text.substr(20, text.size() - 21);
        if (frac.empty() || frac.size() > 9) {
            return std::nullopt;
        }
        std::int64_t mul = 100000000LL;
        for (const char c : frac) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            frac_ns += static_cast<std::int64_t>(c - '0') * mul;
            mul /= 10;
        }
    }
    const std::int64_t days =
        days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const std::int64_t total_sec = days * 86400LL + static_cast<std::int64_t>(hour) * 3600LL +
                                   static_cast<std::int64_t>(minute) * 60LL +
                                   static_cast<std::int64_t>(second);
    if (total_sec < 0 ||
        total_sec > (std::numeric_limits<std::int64_t>::max() - frac_ns) / 1000000000LL) {
        return std::nullopt;
    }
    return WallTime{std::chrono::nanoseconds{total_sec * 1000000000LL + frac_ns}};
}

std::optional<std::chrono::nanoseconds> parse_duration_query(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t split = 0;
    while (split < text.size() && text[split] >= '0' && text[split] <= '9') {
        ++split;
    }
    if (split == 0) {
        return std::nullopt;
    }
    const auto digits = parse_u64_decimal(text.substr(0, split));
    if (!digits) {
        return std::nullopt;
    }
    const auto unit = text.substr(split);
    std::uint64_t scale = 0;
    if (unit.empty() || unit == "ms") {
        scale = 1000000ULL;
    } else if (unit == "ns") {
        scale = 1ULL;
    } else if (unit == "us") {
        scale = 1000ULL;
    } else if (unit == "s") {
        scale = 1000000000ULL;
    } else {
        return std::nullopt;
    }
    const auto max_ns = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (*digits > max_ns / scale) {
        return std::nullopt;
    }
    return std::chrono::nanoseconds{static_cast<std::int64_t>(*digits * scale)};
}

bool is_visible_ascii(std::string_view text, std::size_t min_len, std::size_t max_len) noexcept {
    if (text.size() < min_len || text.size() > max_len) {
        return false;
    }
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x21 || byte > 0x7E) {
            return false;
        }
    }
    return true;
}

}  // namespace rodaq::server
