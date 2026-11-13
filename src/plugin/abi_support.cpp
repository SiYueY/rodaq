#include "abi_support.hpp"

#include <rodaq/plugin/interface.hpp>

#include <algorithm>
#include <cstddef>

namespace rodaq::plugin {
namespace {

thread_local std::size_t managed_call_depth = 0u;

}  // namespace

StatusCode status_code_from_abi(rodaq_status_code_t code) noexcept {
    switch (code) {
        case RODAQ_STATUS_OK:
            return StatusCode::Ok;
        case RODAQ_STATUS_INVALID_ARGUMENT:
            return StatusCode::InvalidArgument;
        case RODAQ_STATUS_INVALID_STATE:
            return StatusCode::InvalidState;
        case RODAQ_STATUS_NOT_FOUND:
            return StatusCode::NotFound;
        case RODAQ_STATUS_CONFLICT:
            return StatusCode::Conflict;
        case RODAQ_STATUS_RESOURCE_EXHAUSTED:
            return StatusCode::ResourceExhausted;
        case RODAQ_STATUS_DEADLINE_EXCEEDED:
            return StatusCode::DeadlineExceeded;
        case RODAQ_STATUS_UNAVAILABLE:
            return StatusCode::Unavailable;
        case RODAQ_STATUS_PERMISSION_DENIED:
            return StatusCode::PermissionDenied;
        case RODAQ_STATUS_DATA_LOSS:
            return StatusCode::DataLoss;
        case RODAQ_STATUS_INTERNAL:
            return StatusCode::Internal;
        default:
            return StatusCode::Internal;
    }
}

bool is_known_status_code(rodaq_status_code_t code) noexcept {
    return code <= RODAQ_STATUS_INTERNAL;
}

bool valid_string(rodaq_string_t string) noexcept {
    if (string.size == 0u) {
        return true;
    }
    if (string.data == nullptr) {
        return false;
    }
    return valid_utf8_text(std::string_view{string.data, string.size});
}

bool valid_bytes(rodaq_bytes_t bytes) noexcept { return bytes.size == 0u || bytes.data != nullptr; }

bool valid_error_buffer(const rodaq_error_t* error) noexcept {
    return interface_support::valid_error_buffer(error);
}

bool valid_utf8_text(std::string_view text) noexcept { return interface_support::valid_utf8(text); }

std::string_view error_message(const rodaq_error_t* error) noexcept {
    return interface_support::error_message(error);
}

Result<void> result_from_abi_call(rodaq_status_code_t code, const rodaq_error_t* error) {
    return interface_support::result_from_abi_call(code, error);
}

std::string_view string_view_from_abi(rodaq_string_t string) noexcept {
    if (!valid_string(string)) {
        return {};
    }
    return std::string_view{string.data, string.size};
}

rodaq_string_t string_to_abi(std::string_view string) noexcept {
    return rodaq_string_t{string.data(), static_cast<uint32_t>(string.size())};
}

void clear_error(rodaq_error_t* error) noexcept { interface_support::clear_error(error); }

void write_error(rodaq_error_t* error, std::string_view message) noexcept {
    interface_support::write_error(error, message);
}

void enter_managed_call() noexcept { ++managed_call_depth; }

void leave_managed_call() noexcept {
    if (managed_call_depth > 0u) {
        --managed_call_depth;
    }
}

bool managed_call_active() noexcept { return managed_call_depth != 0u; }

}  // namespace rodaq::plugin
