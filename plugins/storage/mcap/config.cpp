#include "config.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "../../../src/persistence/json.hpp"

namespace rodaq::mcap_storage {

Result<void> validate_config(std::string_view input) {
    auto parsed = persistence::parse_json(input, {4096, 4, 256, 16, 1, 64});
    if (!parsed.ok()) {
        return parsed.status();
    }
    const auto& config = parsed.value();
    if (!config.is_object() || !config.contains("format_version")) {
        return Status(StatusCode::InvalidArgument, {});
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
            return Status(StatusCode::InvalidArgument, {});
        }
    }
    return {};
}

}  // namespace rodaq::mcap_storage
