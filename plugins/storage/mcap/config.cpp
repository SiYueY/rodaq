#include "config.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace rodaq::mcap_storage {

Result<void> validate_config(std::string_view input) {
    if (input.empty() || input.size() > 4096) {
        return Status(StatusCode::InvalidArgument, "Invalid MCAP config size");
    }
    nlohmann::json config;
    try {
        config = nlohmann::json::parse(input.begin(), input.end());
    } catch (const nlohmann::json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed MCAP config JSON");
    }
    if (!config.is_object() || !config.contains("format_version")) {
        return Status(StatusCode::InvalidArgument, "Missing MCAP format_version");
    }
    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kStrings{{
        {"format_version", "1.0"},
        {"compression", "none"},
        {"chunk_mode", "none"},
        {"rosbag2_metadata", "legacy_ros_distro"},
    }};
    for (const auto& item : config.items()) {
        bool valid = false;
        for (const auto& field : kStrings) {
            valid = valid || (item.key() == field.first && item.value().is_string() &&
                              item.value().get_ref<const std::string&>() == field.second);
        }
        if (item.key() == "chunk_crc" || item.key() == "message_index" || item.key() == "summary") {
            valid = item.value().is_boolean() && !item.value().get<bool>();
        }
        if (!valid) {
            return Status(StatusCode::InvalidArgument, "Unsupported MCAP config field/value");
        }
    }
    return {};
}

}  // namespace rodaq::mcap_storage
