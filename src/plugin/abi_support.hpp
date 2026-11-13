#pragma once

#include <rodaq/plugin/c/abi.h>
#include <rodaq/result.hpp>
#include <rodaq/status.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rodaq::plugin {

[[nodiscard]] StatusCode status_code_from_abi(rodaq_status_code_t code) noexcept;
[[nodiscard]] bool is_known_status_code(rodaq_status_code_t code) noexcept;

[[nodiscard]] bool valid_string(rodaq_string_t string) noexcept;
[[nodiscard]] bool valid_bytes(rodaq_bytes_t bytes) noexcept;
[[nodiscard]] bool valid_error_buffer(const rodaq_error_t* error) noexcept;
[[nodiscard]] bool valid_utf8_text(std::string_view text) noexcept;
[[nodiscard]] std::string_view string_view_from_abi(rodaq_string_t string) noexcept;
[[nodiscard]] rodaq_string_t string_to_abi(std::string_view string) noexcept;
[[nodiscard]] std::string_view error_message(const rodaq_error_t* error) noexcept;
[[nodiscard]] Result<void> result_from_abi_call(
    rodaq_status_code_t code, const rodaq_error_t* error);

void clear_error(rodaq_error_t* error) noexcept;
void write_error(rodaq_error_t* error, std::string_view message) noexcept;
void enter_managed_call() noexcept;
void leave_managed_call() noexcept;
[[nodiscard]] bool managed_call_active() noexcept;

}  // namespace rodaq::plugin
