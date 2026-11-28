#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <rodaq/c/plugin.h>
#include <rodaq/status.hpp>

// Small shared boundary primitives used by SDK trampolines and host wrappers.
namespace rodaq::plugin::abi {

/** Longest complete valid UTF-8 prefix within capacity; no allocation or overread. */
inline std::size_t utf8_prefix(std::string_view text, std::size_t capacity) noexcept {
    const auto limit = std::min(text.size(), capacity);
    std::size_t offset = 0;
    while (offset < limit) {
        const auto first = static_cast<unsigned char>(text[offset]);
        std::size_t width = 0;
        if (first <= 0x7f) {
            width = 1;
        } else if (first >= 0xc2 && first <= 0xdf) {
            width = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            width = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            width = 4;
        } else {
            break;
        }
        if (width > limit - offset) {
            break;
        }
        for (std::size_t i = 1; i < width; ++i) {
            const auto byte = static_cast<unsigned char>(text[offset + i]);
            if (byte < 0x80 || byte > 0xbf) {
                return offset;
            }
        }
        if (width >= 3) {
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
                (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) {
                break;
            }
        }
        offset += width;
    }
    return offset;
}

inline bool valid_string(rodaq_string_t text) noexcept {
    if (text.size == 0) {
        return true;
    }
    if (text.data == nullptr) {
        return false;
    }
    return utf8_prefix({text.data, text.size}, text.size) == text.size;
}

/** Check the published prefix before touching any business field. */
template <typename T>
bool readable(const T* value, std::size_t required_size = sizeof(T)) noexcept {
    if (value == nullptr || reinterpret_cast<std::uintptr_t>(value) % alignof(T) != 0) {
        return false;
    }
    rodaq_struct_header_t header{};
    std::memcpy(&header, value, sizeof(header));
    return header.struct_size >= required_size;
}

/** Captured from caller-owned storage, never from provider-modified descriptors. */
struct BufferSnapshot {
    std::uint8_t* data;
    std::uint32_t capacity;
};

inline bool valid_error_input(const rodaq_error_t* error) noexcept {
    return error == nullptr || ((error->message.data == nullptr) == (error->message.capacity == 0));
}

inline bool valid_error(
    rodaq_status_code_t status, const rodaq_error_t& error, BufferSnapshot expected) noexcept {
    const auto& message = error.message;
    // Identity/capacity comparisons MUST precede every access to message data.
    if (!valid_error_input(&error) || message.data != expected.data ||
        message.capacity != expected.capacity ||
        (message.capacity != 0 && message.data == nullptr) || message.size > message.capacity ||
        (status == RODAQ_STATUS_OK && message.size != 0)) {
        return false;
    }
    if (message.size == 0) {
        return true;
    }
    const auto* text = reinterpret_cast<const char*>(expected.data);
    return utf8_prefix({text, message.size}, message.size) == message.size;
}

inline bool known_status(rodaq_status_code_t status) noexcept {
    return status <= RODAQ_STATUS_INTERNAL;
}

inline bool valid_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > 255) {
        return false;
    }
    for (const char byte : value) {
        if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
              byte == '_' || byte == '-')) {
            return false;
        }
    }
    return true;
}

inline bool valid_query(const rodaq_interface_query_t& query) noexcept {
    return query.interface_id.data != nullptr && query.interface_id.size <= 255 &&
           valid_identifier({query.interface_id.data, query.interface_id.size}) &&
           query.min_minor <= query.max_minor;
}

inline bool default_invalid(const rodaq_interface_reference_t& reference) noexcept {
    return reference.context == nullptr && reference.vtable == nullptr &&
           reference.vtable_size == 0 && reference.negotiated_version.major == 0 &&
           reference.negotiated_version.minor == 0;
}

/** Non-OK primary status survives invalid diagnostics. Unknown/invalid OK fail closed. */
inline Status status_from_abi(
    rodaq_status_code_t code, const rodaq_error_t& error, BufferSnapshot expected) noexcept {
    try {
        if (!known_status(code)) {
            return Status(StatusCode::Internal, "Unknown ABI status");
        }
        if (!valid_error(code, error, expected)) {
            return Status(
                code == RODAQ_STATUS_OK ? StatusCode::Internal : static_cast<StatusCode>(code),
                "Invalid ABI diagnostic buffer");
        }
        if (code == RODAQ_STATUS_OK) {
            return Status{};
        }
        std::string message;
        if (error.message.size != 0) {
            message.assign(reinterpret_cast<const char*>(expected.data), error.message.size);
        }
        return Status(static_cast<StatusCode>(code), std::move(message));
    } catch (...) {
        return Status(
            known_status(code) && code != RODAQ_STATUS_OK ? static_cast<StatusCode>(code)
                                                          : StatusCode::Internal,
            {});
    }
}

/** Preserve caller data/capacity; truncate only at complete UTF-8 code points. */
inline void write_error(rodaq_error_t* error, std::string_view message) noexcept {
    if (error == nullptr) {
        return;
    }
    error->message.size = 0;
    if (error->message.data == nullptr || error->message.capacity == 0) {
        return;
    }
    const auto count = utf8_prefix(message, error->message.capacity);
    if (count != 0) {
        std::memmove(error->message.data, message.data(), count);
    }
    error->message.size = static_cast<std::uint32_t>(count);
}

/** Do not read even the header until the external readable-size contract allows it. */
template <typename Vtable>
std::size_t readable_vtable_size(const rodaq_interface_reference_t& reference) noexcept {
    if (reference.context == nullptr || reference.vtable == nullptr ||
        reference.vtable_size < sizeof(rodaq_struct_header_t) ||
        reinterpret_cast<std::uintptr_t>(reference.vtable) % alignof(Vtable) != 0) {
        return 0;
    }
    // memcpy avoids aliasing a possibly older prefix as a new complete object.
    rodaq_struct_header_t header{};
    std::memcpy(&header, reference.vtable, sizeof(header));
    if (header.struct_size < sizeof(header)) {
        return 0;
    }
    return std::min(
        {std::size_t(reference.vtable_size), std::size_t(header.struct_size), sizeof(Vtable)});
}

inline bool readable_slot(std::size_t readable, std::size_t offset, std::size_t size) noexcept {
    return offset <= readable && size <= readable - offset;
}

template <typename Interface>
bool copy_vtable(
    const rodaq_interface_reference_t& reference, typename Interface::Vtable& table) noexcept {
    static_assert(std::is_trivially_copyable_v<typename Interface::Vtable>);
    static_assert(Interface::minimum_required_prefix_size() >= sizeof(rodaq_struct_header_t));
    static_assert(Interface::minimum_required_prefix_size() <= sizeof(typename Interface::Vtable));
    static_assert(noexcept(Interface::valid_vtable(table, std::uint16_t{})));
    const auto readable = readable_vtable_size<typename Interface::Vtable>(reference);
    if (reference.negotiated_version.major != Interface::kMajor ||
        reference.negotiated_version.minor > Interface::kMinor ||
        readable < Interface::minimum_required_prefix_size()) {
        return false;
    }
    table = {};
    std::memcpy(&table, reference.vtable, readable);
    return Interface::valid_vtable(table, reference.negotiated_version.minor);
}

}  // namespace rodaq::plugin::abi
