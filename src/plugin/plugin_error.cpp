#include "plugin/plugin_error.hpp"

#include <array>
#include <string>
#include <string_view>

namespace rodaq::plugin {
namespace {

constexpr std::array<ErrorCode, 10> kStatusToErrorCode = {
    ErrorCode::InternalError, ErrorCode::InvalidArgument, ErrorCode::InvalidState,
    ErrorCode::NotFound,      ErrorCode::Conflict,        ErrorCode::ResourceExhausted,
    ErrorCode::Unavailable,   ErrorCode::StorageError,    ErrorCode::Timeout,
    ErrorCode::InternalError,
};

ErrorCode map_status(rodaq_status status) {
    if (status >= kStatusToErrorCode.size()) {
        return ErrorCode::InternalError;
    }
    return kStatusToErrorCode[status];
}

std::string_view bounded_view(const char* data, uint32_t size, uint32_t capacity) {
    if (size == 0) {
        return {};
    }
    if (data == nullptr || size > capacity) {
        return {};
    }
    return std::string_view(data, size);
}

}  // namespace

Result<void> result_from_status(
    rodaq_status status, const rodaq_error* status_error, std::string_view fallback_domain) {
    if (status == RODAQ_STATUS_OK) {
        return {};
    }

    ErrorCode code = map_status(status);
    std::string domain(fallback_domain);
    std::int64_t native_code = 0;
    std::string message = "plugin ABI call failed";

    if (status_error != nullptr) {
        const std::string_view status_domain = bounded_view(
            status_error->domain, status_error->domain_size, RODAQ_ERROR_DOMAIN_CAPACITY);
        const std::string_view status_message = bounded_view(
            status_error->message, status_error->message_size, RODAQ_ERROR_MESSAGE_CAPACITY);

        if (!status_domain.empty()) {
            domain.assign(status_domain);
        }
        if (!status_message.empty()) {
            message.assign(status_message);
        }
        native_code = status_error->native_code;
    }

    return nonstd::make_unexpected(Error(code, std::move(domain), native_code, std::move(message)));
}

Error invalid_argument_error(std::string message) {
    return Error(ErrorCode::InvalidArgument, "plugin", 0, std::move(message));
}

Error internal_error(std::string message) {
    return Error(ErrorCode::InternalError, "plugin", 0, std::move(message));
}

}  // namespace rodaq::plugin
