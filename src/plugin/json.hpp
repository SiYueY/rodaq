#pragma once

#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rodaq::plugin {

class JsonValue final {
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };

    using Array = std::vector<JsonValue>;
    using Object = std::vector<std::pair<std::string, JsonValue>>;

    JsonValue() = default;

    [[nodiscard]] static JsonValue boolean(bool value);
    [[nodiscard]] static JsonValue number(double value);
    [[nodiscard]] static JsonValue string(std::string value);
    [[nodiscard]] static JsonValue array(Array value);
    [[nodiscard]] static JsonValue object(Object value);

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_boolean() const noexcept { return type_ == Type::Boolean; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }
    [[nodiscard]] bool as_boolean() const noexcept { return boolean_; }
    [[nodiscard]] double as_number() const noexcept { return number_; }
    [[nodiscard]] const std::string& as_string() const noexcept { return string_; }
    [[nodiscard]] const Array& as_array() const noexcept { return array_; }
    [[nodiscard]] const Object& as_object() const noexcept { return object_; }

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

[[nodiscard]] Result<JsonValue> parse_strict_json(
    std::string_view input, std::size_t max_depth = 32);

[[nodiscard]] bool valid_utf8(std::string_view input) noexcept;

}  // namespace rodaq::plugin
