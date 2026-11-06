#ifndef RODAQ_PLUGIN_ABI_SUPPORT_HPP_
#define RODAQ_PLUGIN_ABI_SUPPORT_HPP_

#include <rodaq/status.hpp>
#include <rodaq/plugin/abi/canonical.h>
#include <rodaq/plugin/abi/common.h>
#include <rodaq/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rodaq {

template <typename Struct>
constexpr std::uint32_t initialized_prefix_size(std::uint32_t capacity) noexcept {
    constexpr std::size_t struct_size = sizeof(Struct);
    return static_cast<std::uint32_t>(capacity < struct_size ? capacity : struct_size);
}

bool is_valid_utf8_without_nul(std::string_view value) noexcept;
bool parse_abi_string(rodaq_string_view value, std::string_view* out_value) noexcept;
bool parse_abi_bytes(
    rodaq_bytes_view value, const std::uint8_t** out_data, std::size_t* out_size) noexcept;
bool is_valid_abi_bool(rodaq_bool value) noexcept;
bool is_valid_abi_status(const rodaq_status& error) noexcept;
bool is_valid_timestamp_values(
    const rodaq_timestamp_value* values, std::uint32_t count, std::uint32_t maximum_count) noexcept;
rodaq_status_code abi_status_from_status_code(StatusCode status_code) noexcept;
void assign_abi_status(
    rodaq_status_code status, std::string_view domain, std::int64_t error_code,
    std::string_view message, rodaq_status* out_status) noexcept;
void assign_abi_status(const Status& status, rodaq_status* out_status) noexcept;
Status status_from_abi(
    rodaq_status_code status, const rodaq_status* status_error,
    std::string_view fallback_domain = "plugin");

}  // namespace rodaq

#endif  // RODAQ_PLUGIN_ABI_SUPPORT_HPP_
