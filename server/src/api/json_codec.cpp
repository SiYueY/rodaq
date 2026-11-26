#include "json_codec.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <utility>
#include <vector>

namespace rodaq::server {
namespace {

class StrictSaxConsumer final : public nlohmann::json::json_sax_t {
public:
    explicit StrictSaxConsumer(StrictJsonLimits limits) : limits_(limits) {}

    bool null() override { return push_scalar(nlohmann::json(nullptr)); }

    bool boolean(bool val) override { return push_scalar(nlohmann::json(val)); }

    bool number_integer(number_integer_t val) override { return push_scalar(nlohmann::json(val)); }

    bool number_unsigned(number_unsigned_t val) override {
        return push_scalar(nlohmann::json(val));
    }

    bool number_float(number_float_t val, const string_t& /*raw*/) override {
        if (!std::isfinite(val)) {
            return fail("invalid_format", current_path(), "non-finite JSON number is rejected");
        }
        return push_scalar(nlohmann::json(val));
    }

    bool string(string_t& val) override {
        if (val.size() > limits_.max_string_bytes) {
            return fail("too_large", current_path(), "JSON string exceeds configured byte limit");
        }
        if (!is_valid_utf8(val)) {
            return fail("invalid_format", current_path(), "JSON string contains invalid UTF-8");
        }
        return push_scalar(nlohmann::json(std::move(val)));
    }

    bool binary(binary_t& /*val*/) override {
        return fail("invalid_type", current_path(), "binary JSON values are not supported");
    }

    bool start_object(std::size_t /*elements*/) override {
        if (!enter_container(nlohmann::json::object())) {
            return false;
        }
        return true;
    }

    bool key(string_t& val) override {
        if (val.size() > limits_.max_string_bytes) {
            return fail("too_large", current_path(), "JSON key exceeds configured byte limit");
        }
        if (!is_valid_utf8(val)) {
            return fail("invalid_format", current_path(), "JSON key contains invalid UTF-8");
        }
        if (frames_.empty() || !frames_.back().value.is_object()) {
            return fail("invalid_format", current_path(), "unexpected JSON object key");
        }
        auto& frame = frames_.back();
        if (frame.value.size() >= limits_.max_members) {
            return fail("too_large", current_path(), "JSON object member count exceeds limit");
        }
        if (frame.value.contains(val)) {
            const std::string dup_path = current_path() + "/" + escape_json_pointer(val);
            return fail("duplicate", dup_path, "duplicate JSON object key");
        }
        frame.pending_key = std::move(val);
        return true;
    }

    bool end_object() override { return leave_container(); }

    bool start_array(std::size_t /*elements*/) override {
        if (!enter_container(nlohmann::json::array())) {
            return false;
        }
        return true;
    }

    bool end_array() override { return leave_container(); }

    bool parse_error(
        std::size_t /*position*/, const std::string& /*last_token*/,
        const nlohmann::json::exception& /*ex*/) override {
        if (failed_) {
            return false;
        }
        return fail("invalid_format", current_path(), "malformed JSON syntax");
    }

    StrictJsonResult finish() {
        StrictJsonResult result;
        if (failed_ || !has_root_ || !frames_.empty()) {
            result.ok = false;
            result.error =
                failed_ ? error_ : JsonParseError{"invalid_format", "", "incomplete JSON input"};
            return result;
        }
        result.ok = true;
        result.value = std::move(root_);
        return result;
    }

private:
    struct Frame {
        nlohmann::json value;
        std::string pending_key;
    };

    bool count_node() {
        if (nodes_ >= limits_.max_nodes) {
            return fail("too_large", current_path(), "JSON node count exceeds configured limit");
        }
        ++nodes_;
        return true;
    }

    std::string current_path() const {
        std::string path;
        for (const auto& frame : frames_) {
            if (frame.value.is_object()) {
                if (!frame.pending_key.empty()) {
                    path.push_back('/');
                    path += escape_json_pointer(frame.pending_key);
                }
            } else if (frame.value.is_array()) {
                path.push_back('/');
                path += std::to_string(frame.value.size());
            }
        }
        return path;
    }

    bool fail(std::string code, std::string path, std::string message) {
        if (!failed_) {
            failed_ = true;
            error_ = {std::move(code), std::move(path), std::move(message)};
        }
        return false;
    }

    bool attach_value(nlohmann::json val) {
        if (frames_.empty()) {
            root_ = std::move(val);
            has_root_ = true;
            return true;
        }
        auto& parent = frames_.back();
        if (parent.value.is_object()) {
            parent.value.emplace(std::move(parent.pending_key), std::move(val));
            parent.pending_key.clear();
            return true;
        }
        if (parent.value.is_array()) {
            if (parent.value.size() >= limits_.max_members) {
                return fail("too_large", current_path(), "JSON array element count exceeds limit");
            }
            parent.value.push_back(std::move(val));
            return true;
        }
        return fail("invalid_format", current_path(), "invalid JSON container state");
    }

    bool push_scalar(nlohmann::json val) {
        if (!count_node()) {
            return false;
        }
        return attach_value(std::move(val));
    }

    bool enter_container(nlohmann::json container) {
        if (!count_node()) {
            return false;
        }
        if (frames_.size() + 1 > limits_.max_depth) {
            return fail("too_large", current_path(), "JSON nesting depth exceeds configured limit");
        }
        frames_.push_back({std::move(container), {}});
        return true;
    }

    bool leave_container() {
        if (frames_.empty()) {
            return fail("invalid_format", "", "unexpected JSON closing delimiter");
        }
        auto completed = std::move(frames_.back().value);
        frames_.pop_back();
        return attach_value(std::move(completed));
    }

    StrictJsonLimits limits_;
    std::vector<Frame> frames_;
    nlohmann::json root_;
    bool has_root_{false};
    bool failed_{false};
    std::size_t nodes_{0};
    JsonParseError error_;
};

std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool parse_fixed_digits(
    std::string_view text, std::size_t pos, std::size_t len, unsigned& out) noexcept {
    if (pos + len > text.size()) {
        return false;
    }
    unsigned value = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const char ch = text[pos + i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(ch - '0');
    }
    out = value;
    return true;
}

bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

unsigned days_in_month(int year, unsigned month) noexcept {
    static constexpr unsigned kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return kDays[month - 1];
}

}  // namespace

StrictJsonResult parse_strict_json(std::string_view input, const StrictJsonLimits& limits) {
    if (input.size() > limits.max_bytes) {
        return {false, {}, {"too_large", "", "request JSON body exceeds byte limit"}};
    }
    if (!is_valid_utf8(input)) {
        return {false, {}, {"invalid_format", "", "request body is not valid UTF-8"}};
    }
    StrictSaxConsumer consumer(limits);
    const bool parsed = nlohmann::json::sax_parse(input.begin(), input.end(), &consumer);
    auto result = consumer.finish();
    if (!parsed && result.ok) {
        result.ok = false;
        result.error = {"invalid_format", "", "malformed JSON input"};
    }
    return result;
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c <= 0x7F) {
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n) {
                return false;
            }
            const auto c1 = static_cast<unsigned char>(text[i + 1]);
            if ((c1 & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (i + 2 >= n) {
                return false;
            }
            const auto c1 = static_cast<unsigned char>(text[i + 1]);
            const auto c2 = static_cast<unsigned char>(text[i + 2]);
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) {
                return false;
            }
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) {
                return false;
            }
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (i + 3 >= n) {
                return false;
            }
            const auto c1 = static_cast<unsigned char>(text[i + 1]);
            const auto c2 = static_cast<unsigned char>(text[i + 2]);
            const auto c3 = static_cast<unsigned char>(text[i + 3]);
            if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
                return false;
            }
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool is_visible_ascii(std::string_view text, std::size_t min_len, std::size_t max_len) noexcept {
    if (text.size() < min_len || text.size() > max_len) {
        return false;
    }
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x21 || byte > 0x7E) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> parse_u64_decimal(std::string_view text) noexcept {
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    if (text.size() > 1 && text[0] == '0') {
        return std::nullopt;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
    }
    std::uint64_t value = 0;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string format_rfc3339_utc(WallTime timestamp) {
    const auto total_ns = timestamp.time_since_epoch().count();
    std::int64_t secs = total_ns / 1000000000LL;
    std::int64_t nanos = total_ns % 1000000000LL;
    if (nanos < 0) {
        nanos += 1000000000LL;
        --secs;
    }
    std::time_t raw_time = static_cast<std::time_t>(secs);
    std::tm utc_tm{};
    gmtime_r(&raw_time, &utc_tm);
    char buf[64]{};
    if (nanos == 0) {
        std::snprintf(
            buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc_tm.tm_year + 1900,
            utc_tm.tm_mon + 1, utc_tm.tm_mday, utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec);
    } else {
        std::snprintf(
            buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ", utc_tm.tm_year + 1900,
            utc_tm.tm_mon + 1, utc_tm.tm_mday, utc_tm.tm_hour, utc_tm.tm_min, utc_tm.tm_sec,
            static_cast<long long>(nanos));
    }
    return std::string(buf);
}

std::optional<WallTime> parse_rfc3339_utc(std::string_view text) noexcept {
    if (text.size() < 20 || text.size() > 35) {
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    unsigned year_u = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (!parse_fixed_digits(text, 0, 4, year_u) || !parse_fixed_digits(text, 5, 2, month) ||
        !parse_fixed_digits(text, 8, 2, day) || !parse_fixed_digits(text, 11, 2, hour) ||
        !parse_fixed_digits(text, 14, 2, minute) || !parse_fixed_digits(text, 17, 2, second)) {
        return std::nullopt;
    }
    const int year = static_cast<int>(year_u);
    if (year < 1970 || year > 2260 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 59) {
        return std::nullopt;
    }
    std::size_t pos = 19;
    std::int64_t nanos = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        const std::size_t frac_start = pos;
        std::int64_t scale = 100000000LL;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (scale == 0) {
                return std::nullopt;
            }
            nanos += static_cast<std::int64_t>(text[pos] - '0') * scale;
            scale /= 10;
            ++pos;
        }
        if (pos == frac_start) {
            return std::nullopt;
        }
    }
    if (pos + 1 != text.size() || text[pos] != 'Z') {
        return std::nullopt;
    }
    const std::int64_t days = days_from_civil(year, month, day);
    const std::int64_t secs = days * 86400LL + static_cast<std::int64_t>(hour) * 3600LL +
                              static_cast<std::int64_t>(minute) * 60LL +
                              static_cast<std::int64_t>(second);
    if (secs > std::numeric_limits<std::int64_t>::max() / 1000000000LL - 1) {
        return std::nullopt;
    }
    return WallTime{std::chrono::nanoseconds{secs * 1000000000LL + nanos}};
}

nlohmann::json format_duration_ns(std::chrono::nanoseconds duration) {
    const auto ns = duration.count() < 0 ? 0LL : duration.count();
    return nlohmann::json{{"value", std::to_string(ns)}, {"unit", "ns"}};
}

nlohmann::json format_duration_ms(std::chrono::nanoseconds duration) {
    const auto ns = duration.count() < 0 ? 0LL : duration.count();
    if (ns % 1000000LL == 0) {
        return nlohmann::json{{"value", std::to_string(ns / 1000000LL)}, {"unit", "ms"}};
    }
    return nlohmann::json{{"value", std::to_string(ns)}, {"unit", "ns"}};
}

std::optional<std::chrono::nanoseconds> parse_duration_object(
    const nlohmann::json& object, std::string& error_code, std::string& error_message) {
    if (!object.is_object()) {
        error_code = "invalid_type";
        error_message = "duration must be an object with value and unit";
        return std::nullopt;
    }
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() != "value" && it.key() != "unit") {
            error_code = "unknown_field";
            error_message = "unknown field in duration object";
            return std::nullopt;
        }
    }
    if (!object.contains("value") || !object.contains("unit")) {
        error_code = "required";
        error_message = "duration requires value and unit";
        return std::nullopt;
    }
    if (!object["value"].is_string() || !object["unit"].is_string()) {
        error_code = "invalid_type";
        error_message = "duration value and unit must be strings";
        return std::nullopt;
    }
    const auto raw = parse_u64_decimal(object["value"].get_ref<const std::string&>());
    if (!raw) {
        error_code = "invalid_format";
        error_message = "duration value must be a canonical unsigned decimal string";
        return std::nullopt;
    }
    const auto& unit = object["unit"].get_ref<const std::string&>();
    std::uint64_t mult = 0;
    if (unit == "ns") {
        mult = 1;
    } else if (unit == "us") {
        mult = 1000ULL;
    } else if (unit == "ms") {
        mult = 1000000ULL;
    } else if (unit == "s") {
        mult = 1000000000ULL;
    } else {
        error_code = "invalid_enum";
        error_message = "duration unit must be one of ns, us, ms, s";
        return std::nullopt;
    }
    constexpr auto kMaxNs = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (*raw > kMaxNs / mult) {
        error_code = "out_of_range";
        error_message = "duration overflows int64 nanoseconds";
        return std::nullopt;
    }
    return std::chrono::nanoseconds{static_cast<std::int64_t>(*raw * mult)};
}

std::optional<std::chrono::nanoseconds> parse_duration_query(std::string_view text) noexcept {
    if (text.size() < 2 || text.size() > 24) {
        return std::nullopt;
    }
    std::uint64_t mult = 0;
    std::string_view digits;
    if (text.size() >= 3 && text.substr(text.size() - 2) == "ns") {
        mult = 1;
        digits = text.substr(0, text.size() - 2);
    } else if (text.size() >= 3 && text.substr(text.size() - 2) == "us") {
        mult = 1000ULL;
        digits = text.substr(0, text.size() - 2);
    } else if (text.size() >= 3 && text.substr(text.size() - 2) == "ms") {
        mult = 1000000ULL;
        digits = text.substr(0, text.size() - 2);
    } else if (text.back() == 's') {
        mult = 1000000000ULL;
        digits = text.substr(0, text.size() - 1);
    } else {
        return std::nullopt;
    }
    const auto raw = parse_u64_decimal(digits);
    if (!raw) {
        return std::nullopt;
    }
    constexpr auto kMaxNs = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (*raw > kMaxNs / mult) {
        return std::nullopt;
    }
    return std::chrono::nanoseconds{static_cast<std::int64_t>(*raw * mult)};
}

std::string escape_json_pointer(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (const char ch : token) {
        if (ch == '~') {
            out += "~0";
        } else if (ch == '/') {
            out += "~1";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

}  // namespace rodaq::server
