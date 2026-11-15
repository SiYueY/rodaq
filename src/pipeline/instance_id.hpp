#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::pipeline {

// Execution-history identity, distinct from Task and Plugin instance identities.
struct InstanceId {
    std::array<std::uint8_t, 16> value{};

    friend bool operator==(const InstanceId& left, const InstanceId& right) noexcept {
        return left.value == right.value;
    }
};

Result<InstanceId> new_instance_id();
Result<InstanceId> parse_instance_id(std::string_view text);
std::string to_string(InstanceId id);
bool valid_instance_id(InstanceId id) noexcept;

}  // namespace rodaq::pipeline
