#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::mcap_storage {

enum class McapProfile { Generic, Ros2 };

struct McapConfig {
    McapProfile profile{McapProfile::Ros2};
    std::optional<std::string> ros_distro;
};

// Strictly parses and validates the qualified V1 MCAP private configuration.
// Rejects unknown fields (including removed rosbag2_metadata), duplicate keys,
// invalid UTF-8/NUL, and ros_distro combined with profile="generic".
Result<McapConfig> parse_config(std::string_view input);

// Convenience wrapper retaining pass/fail validation contract.
Result<void> validate_config(std::string_view input);

}  // namespace rodaq::mcap_storage
