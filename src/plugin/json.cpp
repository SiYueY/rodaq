#include "json.hpp"

#include <rodaq/status.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace rodaq::plugin {
namespace {

class Parser final {
public:
    Parser(std::string_view input, std::size_t max_depth) : input_(input), max_depth_(max_depth) {}

    Result<JsonValue> parse() {
        auto value = parse_value(0u);
        if (!value.ok()) {
            return value.status();
        }
        skip_whitespace();
        if (offset_ != input_.size()) {
            return error("trailing characters after JSON value");
        }
        return std::move(value).value();
    }

private:
    Result<JsonValue> parse_value(std::size_t depth) {
        if (depth > max_depth_) {
            return error("JSON nesting limit exceeded");
        }
        skip_whitespace();
        if (offset_ >= input_.size()) {
            return error("unexpected end of JSON input");
        }

        switch (input_[offset_]) {
            case '{':
                return parse_object(depth);
            case '[':
                return parse_array(depth);
            case '"': {
                auto value = parse_string();
                if (!value.ok()) {
                    return value.status();
                }
                return JsonValue::string(std::move(value).value());
            }
            case 't':
                return parse_literal("true", JsonValue::boolean(true));
            case 'f':
                return parse_literal("false", JsonValue::boolean(false));
            case 'n':
                return parse_literal("null", JsonValue{});
            default:
                return parse_number();
        }
    }

    Result<JsonValue> parse_object(std::size_t depth) {
        ++offset_;
        skip_whitespace();
        JsonValue::Object object;
        if (consume('}')) {
            return JsonValue::object(std::move(object));
        }

        while (true) {
            skip_whitespace();
            if (offset_ >= input_.size() || input_[offset_] != '"') {
                return error("JSON object key must be a string");
            }
            auto key = parse_string();
            if (!key.ok()) {
                return key.status();
            }
            for (const auto& member : object) {
                if (member.first == key.value()) {
                    return error("duplicate JSON object key");
                }
            }
            skip_whitespace();
            if (!consume(':')) {
                return error("missing ':' after JSON object key");
            }
            auto value = parse_value(depth + 1u);
            if (!value.ok()) {
                return value.status();
            }
            object.emplace_back(std::move(key).value(), std::move(value).value());
            skip_whitespace();
            if (consume('}')) {
                return JsonValue::object(std::move(object));
            }
            if (!consume(',')) {
                return error("missing ',' in JSON object");
            }
        }
    }

    Result<JsonValue> parse_array(std::size_t depth) {
        ++offset_;
        skip_whitespace();
        JsonValue::Array array;
        if (consume(']')) {
            return JsonValue::array(std::move(array));
        }
        while (true) {
            auto value = parse_value(depth + 1u);
            if (!value.ok()) {
                return value.status();
            }
            array.push_back(std::move(value).value());
            skip_whitespace();
            if (consume(']')) {
                return JsonValue::array(std::move(array));
            }
            if (!consume(',')) {
                return error("missing ',' in JSON array");
            }
        }
    }

    Result<std::string> parse_string() {
        ++offset_;
        std::string value;
        while (offset_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[offset_++]);
            if (character == '"') {
                if (!valid_utf8(value)) {
                    return error("JSON string is not valid UTF-8");
                }
                return value;
            }
            if (character < 0x20u) {
                return error("unescaped control character in JSON string");
            }
            if (character != '\\') {
                value.push_back(static_cast<char>(character));
                continue;
            }
            if (offset_ >= input_.size()) {
                return error("truncated JSON escape");
            }
            const char escape = input_[offset_++];
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escape);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'u': {
                    auto codepoint = parse_hex_quad();
                    if (!codepoint.ok()) {
                        return codepoint.status();
                    }
                    std::uint32_t scalar = codepoint.value();
                    if (scalar >= 0xd800u && scalar <= 0xdbffu) {
                        if (offset_ + 2u > input_.size() || input_[offset_] != '\\' ||
                            input_[offset_ + 1u] != 'u') {
                            return error("missing low surrogate");
                        }
                        offset_ += 2u;
                        auto low = parse_hex_quad();
                        if (!low.ok() || low.value() < 0xdc00u || low.value() > 0xdfffu) {
                            return error("invalid low surrogate");
                        }
                        scalar = 0x10000u + ((scalar - 0xd800u) << 10u) + (low.value() - 0xdc00u);
                    } else if (scalar >= 0xdc00u && scalar <= 0xdfffu) {
                        return error("unexpected low surrogate");
                    }
                    append_utf8(value, scalar);
                    break;
                }
                default:
                    return error("invalid JSON escape");
            }
        }
        return error("unterminated JSON string");
    }

    Result<std::uint32_t> parse_hex_quad() {
        if (offset_ + 4u > input_.size()) {
            return error("truncated unicode escape");
        }
        std::uint32_t value = 0u;
        for (std::size_t index = 0u; index < 4u; ++index) {
            const char digit = input_[offset_++];
            value <<= 4u;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                return error("invalid unicode escape");
            }
        }
        return value;
    }

    static void append_utf8(std::string& output, std::uint32_t scalar) {
        if (scalar <= 0x7fu) {
            output.push_back(static_cast<char>(scalar));
        } else if (scalar <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (scalar >> 6u)));
            output.push_back(static_cast<char>(0x80u | (scalar & 0x3fu)));
        } else if (scalar <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (scalar >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((scalar >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (scalar & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (scalar >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((scalar >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((scalar >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (scalar & 0x3fu)));
        }
    }

    Result<JsonValue> parse_number() {
        const auto start = offset_;
        consume('-');
        if (offset_ >= input_.size()) {
            return error("invalid JSON number");
        }
        if (input_[offset_] == '0') {
            ++offset_;
        } else if (input_[offset_] >= '1' && input_[offset_] <= '9') {
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        } else {
            return error("invalid JSON number");
        }

        if (consume('.')) {
            if (offset_ >= input_.size() || input_[offset_] < '0' || input_[offset_] > '9') {
                return error("invalid JSON fraction");
            }
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }
        if (offset_ < input_.size() && (input_[offset_] == 'e' || input_[offset_] == 'E')) {
            ++offset_;
            if (offset_ < input_.size() && (input_[offset_] == '+' || input_[offset_] == '-')) {
                ++offset_;
            }
            if (offset_ >= input_.size() || input_[offset_] < '0' || input_[offset_] > '9') {
                return error("invalid JSON exponent");
            }
            while (offset_ < input_.size() && input_[offset_] >= '0' && input_[offset_] <= '9') {
                ++offset_;
            }
        }

        const std::string token{input_.substr(start, offset_ - start)};
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(value)) {
            return error("JSON number is out of range");
        }
        return JsonValue::number(value);
    }

    Result<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
        if (input_.substr(offset_, literal.size()) != literal) {
            return error("invalid JSON literal");
        }
        offset_ += literal.size();
        return value;
    }

    void skip_whitespace() noexcept {
        while (offset_ < input_.size()) {
            const char character = input_[offset_];
            if (character != ' ' && character != '\n' && character != '\r' && character != '\t') {
                break;
            }
            ++offset_;
        }
    }

    bool consume(char expected) noexcept {
        if (offset_ < input_.size() && input_[offset_] == expected) {
            ++offset_;
            return true;
        }
        return false;
    }

    static Status error(std::string message) {
        return Status{StatusCode::InvalidArgument, std::move(message)};
    }

    std::string_view input_;
    std::size_t max_depth_;
    std::size_t offset_ = 0u;
};

}  // namespace

JsonValue JsonValue::boolean(bool value) {
    JsonValue result;
    result.type_ = Type::Boolean;
    result.boolean_ = value;
    return result;
}

JsonValue JsonValue::number(double value) {
    JsonValue result;
    result.type_ = Type::Number;
    result.number_ = value;
    return result;
}

JsonValue JsonValue::string(std::string value) {
    JsonValue result;
    result.type_ = Type::String;
    result.string_ = std::move(value);
    return result;
}

JsonValue JsonValue::array(Array value) {
    JsonValue result;
    result.type_ = Type::Array;
    result.array_ = std::move(value);
    return result;
}

JsonValue JsonValue::object(Object value) {
    JsonValue result;
    result.type_ = Type::Object;
    result.object_ = std::move(value);
    return result;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& member : object_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

Result<JsonValue> parse_strict_json(std::string_view input, std::size_t max_depth) {
    if (!valid_utf8(input)) {
        return Status{StatusCode::InvalidArgument, "JSON input is not valid UTF-8"};
    }
    return Parser{input, max_depth}.parse();
}

bool valid_utf8(std::string_view input) noexcept {
    std::size_t index = 0u;
    while (index < input.size()) {
        const auto first = static_cast<unsigned char>(input[index++]);
        if (first <= 0x7fu) {
            continue;
        }

        std::size_t continuation_count = 0u;
        std::uint32_t scalar = 0u;
        if ((first & 0xe0u) == 0xc0u) {
            continuation_count = 1u;
            scalar = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation_count = 2u;
            scalar = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation_count = 3u;
            scalar = first & 0x07u;
        } else {
            return false;
        }
        if (index + continuation_count > input.size()) {
            return false;
        }
        for (std::size_t count = 0u; count < continuation_count; ++count) {
            const auto continuation = static_cast<unsigned char>(input[index++]);
            if ((continuation & 0xc0u) != 0x80u) {
                return false;
            }
            scalar = (scalar << 6u) | (continuation & 0x3fu);
        }
        if ((continuation_count == 1u && scalar < 0x80u) ||
            (continuation_count == 2u && scalar < 0x800u) ||
            (continuation_count == 3u && scalar < 0x10000u) || scalar > 0x10ffffu ||
            (scalar >= 0xd800u && scalar <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

}  // namespace rodaq::plugin
