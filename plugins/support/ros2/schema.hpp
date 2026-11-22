#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::ros2 {

struct Schema {
    std::string name;
    std::string encoding;
    std::string text;
};

struct SchemaLimits {
    std::size_t max_bytes{1024 * 1024};
    std::size_t max_definitions{256};
    std::size_t max_depth{32};
};

// Deterministic shared ROS2/Fast-DDS support, not a Framework/public ABI.
// Reads installed resources on preparation/control capacity, never a callback.
// DFS visits lexically sorted canonical dependencies once. Definitions use LF
// with one final LF; MSG dependency labels retain standard package/Type syntax.
Result<Schema> resolve_schema(std::string_view canonical_type, SchemaLimits limits = {});
bool valid_type_name(std::string_view type) noexcept;

}  // namespace rodaq::ros2
