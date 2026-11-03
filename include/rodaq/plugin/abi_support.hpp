#ifndef RODAQ_PLUGIN_ABI_SUPPORT_HPP_
#define RODAQ_PLUGIN_ABI_SUPPORT_HPP_

#include <rodaq/error.hpp>
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
bool is_valid_timestamp_values(const rodaq_timestamp_value* values, std::uint32_t count) noexcept;
rodaq_status status_from_error_code(ErrorCode code) noexcept;
void assign_rodaq_error(
    rodaq_status status, std::string_view domain, std::int64_t native_code,
    std::string_view message, rodaq_error* out_error) noexcept;
void assign_rodaq_error(const Error& error, rodaq_error* out_error) noexcept;
Result<void> result_from_status(
    rodaq_status status, const rodaq_error* status_error,
    std::string_view fallback_domain = "plugin");

}  // namespace rodaq

#endif  // RODAQ_PLUGIN_ABI_SUPPORT_HPP_
