#include "config.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include "strict_json.hpp"

namespace rodaq::mcap_storage {
namespace {

bool valid_ros_distro(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (first < 'a' || first > 'z') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const auto byte = static_cast<unsigned char>(ch);
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' ||
               byte == '-';
    });
}

}  // namespace

Result<McapConfig> parse_config(std::string_view input) {
    if (input.empty() || input.size() > 4096) {
        return Status(StatusCode::InvalidArgument, "Invalid MCAP config size");
    }
    auto document = internal::parse_strict_json(input, {4096, 8, 256});
    if (!document.ok()) {
        return document.status();
    }
    const auto& config = document.value();
    if (!config.is_object() || !config.contains("format_version")) {
        return Status(StatusCode::InvalidArgument, "Missing MCAP format_version");
    }
    constexpr std::array<std::pair<std::string_view, std::string_view>, 3> kFixedStrings{{
        {"format_version", "1.0"},
        {"compression", "none"},
        {"chunk_mode", "none"},
    }};
    McapConfig parsed;
    for (const auto& item : config.items()) {
        const auto& key = item.key();
        const auto& value = item.value();
        bool valid = false;
        for (const auto& field : kFixedStrings) {
            if (key == field.first) {
                valid = value.is_string() && value.get_ref<const std::string&>() == field.second;
            }
        }
        if (key == "chunk_crc" || key == "message_index" || key == "summary") {
            valid = value.is_boolean() && !value.get<bool>();
        } else if (key == "profile") {
            if (value.is_string()) {
                const auto& prof = value.get_ref<const std::string&>();
                if (prof == "generic") {
                    parsed.profile = McapProfile::Generic;
                    valid = true;
                } else if (prof == "ros2") {
                    parsed.profile = McapProfile::Ros2;
                    valid = true;
                }
            }
        } else if (key == "ros_distro") {
            if (value.is_string() && valid_ros_distro(value.get_ref<const std::string&>())) {
                parsed.ros_distro = value.get<std::string>();
                valid = true;
            }
        }
        if (!valid) {
            return Status(StatusCode::InvalidArgument, "Unsupported MCAP config field/value");
        }
    }
    if (parsed.profile == McapProfile::Generic && parsed.ros_distro.has_value()) {
        return Status(
            StatusCode::InvalidArgument, "MCAP generic profile forbids ros_distro configuration");
    }
    return parsed;
}

Result<void> validate_config(std::string_view input) {
    auto parsed = parse_config(input);
    if (!parsed.ok()) {
        return parsed.status();
    }
    return {};
}

}  // namespace rodaq::mcap_storage
