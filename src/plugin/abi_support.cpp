#include <rodaq/plugin/abi_support.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace rodaq {
namespace {

template <typename Character>
void copy_bounded(
    Character* destination, std::uint32_t capacity, std::string_view source,
    std::uint32_t* out_size) noexcept {
    const std::size_t copied =
        std::min<std::size_t>(source.size(), static_cast<std::size_t>(capacity));
    if (copied != 0) {
        std::memcpy(destination, source.data(), copied);
    }
    *out_size = static_cast<std::uint32_t>(copied);
}

}  // namespace

bool is_valid_utf8_without_nul(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        if (first == 0) {
            return false;
        }
        if (first < 0x80) {
            ++index;
            continue;
        }

        if (first >= 0xC2 && first <= 0xDF) {
            if (index + 1 >= value.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(value[index + 1]);
            if ((second & 0xC0U) != 0x80U) {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xE0 && first <= 0xEF) {
            if (index + 2 >= value.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(value[index + 1]);
            const unsigned char third = static_cast<unsigned char>(value[index + 2]);
            if ((third & 0xC0U) != 0x80U) {
                return false;
            }
            if (first == 0xE0 && (second < 0xA0 || second > 0xBF)) {
                return false;
            }
            if (first == 0xED && (second < 0x80 || second > 0x9F)) {
                return false;
            }
            if (first != 0xE0 && first != 0xED && (second & 0xC0U) != 0x80U) {
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xF0 && first <= 0xF4) {
            if (index + 3 >= value.size()) {
                return false;
            }
            const unsigned char second = static_cast<unsigned char>(value[index + 1]);
            const unsigned char third = static_cast<unsigned char>(value[index + 2]);
            const unsigned char fourth = static_cast<unsigned char>(value[index + 3]);
            if (first == 0xF0 && (second < 0x90 || second > 0xBF)) {
                return false;
            }
            if (first == 0xF4 && (second < 0x80 || second > 0x8F)) {
                return false;
            }
            if (first != 0xF0 && first != 0xF4 && (second & 0xC0U) != 0x80U) {
                return false;
            }
            if ((third & 0xC0U) != 0x80U || (fourth & 0xC0U) != 0x80U) {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }
    return true;
}

bool parse_abi_string(rodaq_string_view value, std::string_view* out_value) noexcept {
    if (out_value == nullptr || (value.size != 0 && value.data == nullptr)) {
        return false;
    }
    if (value.size > std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    const std::string_view parsed(
        value.data == nullptr ? "" : value.data, static_cast<std::size_t>(value.size));
    if (!is_valid_utf8_without_nul(parsed)) {
        return false;
    }
    *out_value = parsed;
    return true;
}

bool parse_abi_bytes(
    rodaq_bytes_view value, const std::uint8_t** out_data, std::size_t* out_size) noexcept {
    if (out_data == nullptr || out_size == nullptr || (value.size != 0 && value.data == nullptr)) {
        return false;
    }
    if (value.size > std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    *out_data = value.data;
    *out_size = static_cast<std::size_t>(value.size);
    return true;
}

bool is_valid_abi_bool(rodaq_bool value) noexcept {
    return value == RODAQ_FALSE || value == RODAQ_TRUE;
}

bool is_valid_abi_status(const rodaq_status& error) noexcept {
    const bool known_status = error.status_code == RODAQ_STATUS_OK ||
                              error.status_code == RODAQ_STATUS_INVALID_ARGUMENT ||
                              error.status_code == RODAQ_STATUS_INVALID_STATE ||
                              error.status_code == RODAQ_STATUS_NOT_FOUND ||
                              error.status_code == RODAQ_STATUS_CONFLICT ||
                              error.status_code == RODAQ_STATUS_RESOURCE_EXHAUSTED ||
                              error.status_code == RODAQ_STATUS_UNAVAILABLE ||
                              error.status_code == RODAQ_STATUS_STORAGE_ERROR ||
                              error.status_code == RODAQ_STATUS_TIMEOUT ||
                              error.status_code == RODAQ_STATUS_INTERNAL_ERROR ||
                              error.status_code == RODAQ_STATUS_UNKNOWN_ERROR;
    if (!known_status || (error.flags & ~RODAQ_STATUS_FLAG_MESSAGE_TRUNCATED) != 0 ||
        error.domain_size > RODAQ_STATUS_DOMAIN_CAPACITY ||
        error.message_size > RODAQ_STATUS_MESSAGE_CAPACITY || error.reserved[0] != 0 ||
        error.reserved[1] != 0) {
        return false;
    }
    const std::string_view domain(error.domain, error.domain_size);
    const std::string_view message(error.message, error.message_size);
    return is_valid_utf8_without_nul(domain) && is_valid_utf8_without_nul(message);
}

bool is_valid_timestamp_values(
    const rodaq_timestamp_value* values, std::uint32_t count,
    std::uint32_t maximum_count) noexcept {
    if (count > maximum_count || (count != 0 && values == nullptr)) {
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        if (!is_valid_abi_bool(values[index].present) || values[index].reserved != 0) {
            return false;
        }
    }
    return true;
}

rodaq_status_code abi_status_from_status_code(StatusCode status_code) noexcept {
    switch (status_code) {
        case StatusCode::Ok:
            return RODAQ_STATUS_OK;
        case StatusCode::InvalidArgument:
            return RODAQ_STATUS_INVALID_ARGUMENT;
        case StatusCode::InvalidState:
            return RODAQ_STATUS_INVALID_STATE;
        case StatusCode::NotFound:
            return RODAQ_STATUS_NOT_FOUND;
        case StatusCode::Conflict:
            return RODAQ_STATUS_CONFLICT;
        case StatusCode::ResourceExhausted:
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;
        case StatusCode::Unavailable:
            return RODAQ_STATUS_UNAVAILABLE;
        case StatusCode::StorageError:
            return RODAQ_STATUS_STORAGE_ERROR;
        case StatusCode::Timeout:
            return RODAQ_STATUS_TIMEOUT;
        case StatusCode::InternalError:
            return RODAQ_STATUS_INTERNAL_ERROR;
        case StatusCode::UnknownError:
            return RODAQ_STATUS_UNKNOWN_ERROR;
    }
    return RODAQ_STATUS_UNKNOWN_ERROR;
}

void assign_abi_status(
    rodaq_status_code status, std::string_view domain, std::int64_t error_code,
    std::string_view message, rodaq_status* out_status) noexcept {
    if (out_status == nullptr) {
        return;
    }

    *out_status = {};
    out_status->status_code = status;
    if (status == RODAQ_STATUS_OK) {
        return;
    }
    out_status->error_code = error_code;
    copy_bounded(
        out_status->domain, RODAQ_STATUS_DOMAIN_CAPACITY, domain, &out_status->domain_size);
    copy_bounded(
        out_status->message, RODAQ_STATUS_MESSAGE_CAPACITY, message, &out_status->message_size);
    if (domain.size() > RODAQ_STATUS_DOMAIN_CAPACITY ||
        message.size() > RODAQ_STATUS_MESSAGE_CAPACITY) {
        out_status->flags |= RODAQ_STATUS_FLAG_MESSAGE_TRUNCATED;
    }
}

void assign_abi_status(const Status& status, rodaq_status* out_status) noexcept {
    assign_abi_status(
        abi_status_from_status_code(status.code()), status.domain(), status.error_code(),
        status.message(), out_status);
}

Status status_from_abi(
    rodaq_status_code status, const rodaq_status* status_error, std::string_view fallback_domain) {
    if (status == RODAQ_STATUS_OK) {
        return {};
    }

    StatusCode status_code = StatusCode::UnknownError;
    switch (status) {
        case RODAQ_STATUS_INVALID_ARGUMENT:
            status_code = StatusCode::InvalidArgument;
            break;
        case RODAQ_STATUS_INVALID_STATE:
            status_code = StatusCode::InvalidState;
            break;
        case RODAQ_STATUS_NOT_FOUND:
            status_code = StatusCode::NotFound;
            break;
        case RODAQ_STATUS_CONFLICT:
            status_code = StatusCode::Conflict;
            break;
        case RODAQ_STATUS_RESOURCE_EXHAUSTED:
            status_code = StatusCode::ResourceExhausted;
            break;
        case RODAQ_STATUS_UNAVAILABLE:
            status_code = StatusCode::Unavailable;
            break;
        case RODAQ_STATUS_STORAGE_ERROR:
            status_code = StatusCode::StorageError;
            break;
        case RODAQ_STATUS_TIMEOUT:
            status_code = StatusCode::Timeout;
            break;
        case RODAQ_STATUS_UNKNOWN_ERROR:
            status_code = StatusCode::UnknownError;
            break;
        case RODAQ_STATUS_INTERNAL_ERROR:
            status_code = StatusCode::InternalError;
            break;
        default:
            break;
    }

    std::string domain(fallback_domain);
    std::int64_t error_code = 0;
    std::string message = "plugin ABI call failed";
    if (status_error != nullptr) {
        rodaq_status normalized = *status_error;
        if (status > RODAQ_STATUS_UNKNOWN_ERROR && status_error->status_code == status) {
            normalized.status_code = RODAQ_STATUS_UNKNOWN_ERROR;
        }
        if (!is_valid_abi_status(normalized) || status_error->status_code != status) {
            return rodaq::Status(
                StatusCode::InternalError, "plugin", 0,
                "Plugin returned a malformed ABI diagnostic");
        }
        if (status_error->domain_size > 0 &&
            status_error->domain_size <= RODAQ_STATUS_DOMAIN_CAPACITY) {
            domain.assign(status_error->domain, status_error->domain_size);
        }
        if (status_error->message_size > 0 &&
            status_error->message_size <= RODAQ_STATUS_MESSAGE_CAPACITY) {
            message.assign(status_error->message, status_error->message_size);
        }
        error_code = status_error->error_code;
    }
    return rodaq::Status(status_code, std::move(domain), error_code, std::move(message));
}

}  // namespace rodaq
