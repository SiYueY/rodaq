#ifndef RODAQ_ABI_SUPPORT_HPP_
#define RODAQ_ABI_SUPPORT_HPP_

#include <rodaq/error.hpp>
#include <rodaq/plugin/abi/common.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rodaq {

bool is_valid_utf8_without_nul(std::string_view value) noexcept;

bool parse_abi_string(rodaq_string_view value, std::string_view* out_value) noexcept;

bool parse_abi_bytes(
    rodaq_bytes_view value, const std::uint8_t** out_data, std::size_t* out_size) noexcept;

rodaq_status status_from_error_code(ErrorCode code) noexcept;

void assign_rodaq_error(
    rodaq_status status, std::string_view domain, std::int64_t native_code,
    std::string_view message, rodaq_error* out_error) noexcept;

void assign_rodaq_error(const Error& error, rodaq_error* out_error) noexcept;

}  // namespace rodaq

#endif  // RODAQ_ABI_SUPPORT_HPP_
