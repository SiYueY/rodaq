#include "abi_support.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

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

rodaq_status status_from_error_code(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidArgument:
            return RODAQ_STATUS_INVALID_ARGUMENT;
        case ErrorCode::InvalidState:
            return RODAQ_STATUS_INVALID_STATE;
        case ErrorCode::NotFound:
            return RODAQ_STATUS_NOT_FOUND;
        case ErrorCode::Conflict:
            return RODAQ_STATUS_CONFLICT;
        case ErrorCode::ResourceExhausted:
            return RODAQ_STATUS_RESOURCE_EXHAUSTED;
        case ErrorCode::Unavailable:
            return RODAQ_STATUS_UNAVAILABLE;
        case ErrorCode::StorageError:
            return RODAQ_STATUS_STORAGE_ERROR;
        case ErrorCode::Timeout:
            return RODAQ_STATUS_TIMEOUT;
        case ErrorCode::InternalError:
            return RODAQ_STATUS_INTERNAL_ERROR;
    }
    return RODAQ_STATUS_INTERNAL_ERROR;
}

void assign_rodaq_error(
    rodaq_status status, std::string_view domain, std::int64_t native_code,
    std::string_view message, rodaq_error* out_error) noexcept {
    if (out_error == nullptr) {
        return;
    }

    *out_error = {};
    out_error->code = status;
    out_error->native_code = native_code;
    copy_bounded(out_error->domain, RODAQ_ERROR_DOMAIN_CAPACITY, domain, &out_error->domain_size);
    copy_bounded(
        out_error->message, RODAQ_ERROR_MESSAGE_CAPACITY, message, &out_error->message_size);
    if (domain.size() > RODAQ_ERROR_DOMAIN_CAPACITY ||
        message.size() > RODAQ_ERROR_MESSAGE_CAPACITY) {
        out_error->flags |= RODAQ_ERROR_FLAG_MESSAGE_TRUNCATED;
    }
}

void assign_rodaq_error(const Error& error, rodaq_error* out_error) noexcept {
    assign_rodaq_error(
        status_from_error_code(error.code()), error.domain(), error.native_code(), error.message(),
        out_error);
}

}  // namespace rodaq
