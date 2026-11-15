#include "json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace rodaq::persistence {
namespace {

struct InvalidCanonicalJson {};

std::u16string utf16(std::string_view text) {
    std::u16string result;
    result.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<std::uint8_t>(text[offset++]);
        std::uint32_t point = first;
        std::size_t continuation = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            point = first & 0x1fu;
            continuation = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            point = first & 0x0fu;
            continuation = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            point = first & 0x07u;
            continuation = 3;
            minimum = 0x10000;
        } else if (first >= 0x80) {
            throw InvalidCanonicalJson{};
        }
        if (continuation > text.size() - offset) {
            throw InvalidCanonicalJson{};
        }
        for (std::size_t index = 0; index < continuation; ++index) {
            const auto byte = static_cast<std::uint8_t>(text[offset++]);
            if ((byte & 0xc0u) != 0x80u) {
                throw InvalidCanonicalJson{};
            }
            point = (point << 6u) | (byte & 0x3fu);
        }
        if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
            throw InvalidCanonicalJson{};
        }
        if (point <= 0xffff) {
            result.push_back(static_cast<char16_t>(point));
        } else {
            point -= 0x10000;
            result.push_back(static_cast<char16_t>(0xd800u + (point >> 10u)));
            result.push_back(static_cast<char16_t>(0xdc00u + (point & 0x3ffu)));
        }
    }
    return result;
}

std::string number(double value) {
    if (!std::isfinite(value)) {
        throw InvalidCanonicalJson{};
    }
    if (value == 0) {
        return "0";
    }
    // fmt 11.2's Dragonbox supplies shortest round-to-even binary64 digits.
    // JCS/ECMAScript has different fixed/scientific notation thresholds.
    auto shortest = fmt::format("{}", std::abs(value));
    auto exponent_at = shortest.find('e');
    int exponent = 0;
    if (exponent_at != std::string::npos) {
        auto start = shortest.data() + exponent_at + 1;
        if (*start == '+') {
            ++start;
        }
        const auto parsed = std::from_chars(start, shortest.data() + shortest.size(), exponent);
        if (parsed.ec != std::errc{} || parsed.ptr != shortest.data() + shortest.size()) {
            throw InvalidCanonicalJson{};
        }
        shortest.resize(exponent_at);
    }
    const auto dot = shortest.find('.');
    auto position = static_cast<int>(dot == std::string::npos ? shortest.size() : dot) + exponent;
    if (dot != std::string::npos) {
        shortest.erase(dot, 1);
    }
    while (shortest.size() > 1 && shortest.front() == '0') {
        shortest.erase(0, 1);
        --position;
    }
    while (shortest.size() > 1 && shortest.back() == '0') {
        shortest.pop_back();
    }
    std::string output = std::signbit(value) ? "-" : "";
    if (position > 0 && position <= 21) {
        const auto before = static_cast<std::size_t>(position);
        if (before >= shortest.size()) {
            output += shortest;
            output.append(before - shortest.size(), '0');
        } else {
            output += shortest.substr(0, before);
            output += '.';
            output += shortest.substr(before);
        }
    } else if (position <= 0 && position > -6) {
        output += "0.";
        output.append(static_cast<std::size_t>(-position), '0');
        output += shortest;
    } else {
        output += shortest.front();
        if (shortest.size() > 1) {
            output += '.';
            output += shortest.substr(1);
        }
        output += 'e';
        if (position - 1 >= 0) {
            output += '+';
        }
        output += std::to_string(position - 1);
    }
    return output;
}

class CanonicalWriter {
public:
    explicit CanonicalWriter(const JsonLimits& limits) : limits_(limits) {}

    std::string write(const Json& value) {
        visit(value, 0);
        return std::move(output_);
    }

private:
    void append(std::string_view text) {
        if (text.size() > limits_.max_bytes - output_.size()) {
            throw InvalidCanonicalJson{};
        }
        output_.append(text);
    }

    void charge(std::string_view text) {
        if (text.size() > limits_.max_string_bytes ||
            text.size() > limits_.max_bytes - input_bytes_) {
            throw InvalidCanonicalJson{};
        }
        input_bytes_ += text.size();
    }

    void quote(std::string_view text) {
        (void)utf16(
            text);  // Programmatic JSON must pass the same Unicode validation as parsed JSON.
        append("\"");
        for (const auto character : text) {
            const auto byte = static_cast<std::uint8_t>(character);
            switch (byte) {
                case '"':
                    append("\\\"");
                    break;
                case '\\':
                    append("\\\\");
                    break;
                case '\b':
                    append("\\b");
                    break;
                case '\t':
                    append("\\t");
                    break;
                case '\n':
                    append("\\n");
                    break;
                case '\f':
                    append("\\f");
                    break;
                case '\r':
                    append("\\r");
                    break;
                default:
                    if (byte < 0x20) {
                        constexpr std::string_view digits = "0123456789abcdef";
                        const char escaped[]{
                            '\\', 'u', '0', '0', digits[byte >> 4u], digits[byte & 0xfu]};
                        append({escaped, sizeof(escaped)});
                    } else {
                        append({&character, 1});
                    }
            }
        }
        append("\"");
    }

    void visit(const Json& value, std::size_t depth) {
        if (++nodes_ > limits_.max_nodes || (value.is_structured() && depth >= limits_.max_depth)) {
            throw InvalidCanonicalJson{};
        }
        if (value.is_object()) {
            object(value, depth);
        } else if (value.is_array()) {
            if (value.size() > limits_.max_array_elements) {
                throw InvalidCanonicalJson{};
            }
            append("[");
            bool first = true;
            for (const auto& entry : value) {
                if (!std::exchange(first, false)) {
                    append(",");
                }
                visit(entry, depth + 1);
            }
            append("]");
        } else if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            charge(text);
            quote(text);
        } else if (value.is_number()) {
            append(number(value.get<double>()));
        } else if (value.is_boolean()) {
            append(value.get<bool>() ? "true" : "false");
        } else if (value.is_null()) {
            append("null");
        } else {
            throw InvalidCanonicalJson{};
        }
    }

    void object(const Json& value, std::size_t depth) {
        if (value.size() > limits_.max_object_members) {
            throw InvalidCanonicalJson{};
        }
        struct Member {
            std::u16string order;
            const std::string* key;
            const Json* value;
        };
        std::vector<Member> members;
        members.reserve(value.size());
        for (auto entry = value.begin(); entry != value.end(); ++entry) {
            charge(entry.key());
            members.push_back({utf16(entry.key()), &entry.key(), &entry.value()});
        }
        std::sort(members.begin(), members.end(), [](const auto& left, const auto& right) {
            return left.order < right.order;
        });
        append("{");
        bool first = true;
        for (const auto& member : members) {
            if (!std::exchange(first, false)) {
                append(",");
            }
            quote(*member.key);
            append(":");
            visit(*member.value, depth + 1);
        }
        append("}");
    }

    const JsonLimits& limits_;
    std::string output_;
    std::size_t nodes_{0};
    std::size_t input_bytes_{0};
};

}  // namespace

Result<std::string> canonical_json(const Json& value, const JsonLimits& limits) {
    if (limits.max_depth == 0 || limits.max_depth > 64 || limits.max_nodes == 0) {
        return Status(StatusCode::InvalidArgument, "Invalid canonical JSON limits");
    }
    try {
        return CanonicalWriter(limits).write(value);
    } catch (const InvalidCanonicalJson&) {
        return Status(StatusCode::InvalidArgument, "Non-JCS JSON or resource bound");
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::persistence
