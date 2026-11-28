#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>

#include <rodaq/c/canonical.h>
#include <rodaq/plugin/abi.hpp>

namespace rodaq::canonical {

/** Validate opaque source identity; never compare compiler padding bytes. */
inline bool valid_source_id(const rodaq_source_id_t& source, bool present) noexcept {
    if (!std::all_of(std::begin(source.reserved), std::end(source.reserved), [](auto byte) {
            return byte == 0;
        })) {
        return false;
    }
    if (!present) {
        return source.size == 0 && std::all_of(
                                       std::begin(source.data), std::end(source.data),
                                       [](auto byte) { return byte == 0; });
    }
    if (source.size == 0 || source.size > RODAQ_SOURCE_ID_DATA_CAPACITY) {
        return false;
    }
    return std::all_of(std::begin(source.data) + source.size, std::end(source.data), [](auto byte) {
        return byte == 0;
    });
}

inline bool valid_text(rodaq_string_t text, std::uint32_t limit, bool token = false) noexcept {
    if (text.size == 0 || text.size > limit || !plugin::abi::valid_string(text) ||
        std::memchr(text.data, 0, text.size) != nullptr) {
        return false;
    }
    return !token || std::all_of(text.data, text.data + text.size, [](unsigned char byte) {
        return byte >= 0x21 && byte <= 0x7e;
    });
}

/** Matches ^[a-z0-9][a-z0-9._/-]{0,63}$ without locale or regex overhead. */
inline bool valid_metadata_format(std::string_view format) noexcept {
    if (format.empty() || format.size() > 64) {
        return false;
    }
    const auto first = static_cast<unsigned char>(format.front());
    const bool first_ok = (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
    if (!first_ok) {
        return false;
    }
    return std::all_of(format.begin() + 1, format.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '.' ||
               byte == '_' || byte == '/' || byte == '-';
    });
}

inline bool valid_descriptor_metadata(const rodaq_descriptor_metadata_t& metadata) noexcept {
    if (!plugin::abi::valid_string(metadata.format) || !plugin::abi::valid_string(metadata.json)) {
        return false;
    }
    if (metadata.format.size == 0 && metadata.json.size == 0) {
        return true;
    }
    if (metadata.format.size == 0 || metadata.json.size == 0) {
        return false;
    }
    return valid_metadata_format({metadata.format.data, metadata.format.size}) &&
           std::memchr(metadata.json.data, 0, metadata.json.size) == nullptr;
}

template <typename Descriptor>
bool valid_descriptor_fields(const Descriptor& descriptor) noexcept {
    return valid_text(descriptor.channel_name, 1024) &&
           valid_text(descriptor.message_encoding, 64, true) &&
           (descriptor.schema_name.size == 0 ? plugin::abi::valid_string(descriptor.schema_name)
                                             : valid_text(descriptor.schema_name, 1024)) &&
           (descriptor.schema_encoding.size == 0
                ? plugin::abi::valid_string(descriptor.schema_encoding)
                : valid_text(descriptor.schema_encoding, 64, true)) &&
           (descriptor.schema.size == 0 || descriptor.schema.data != nullptr) &&
           valid_descriptor_metadata(descriptor.metadata);
}

}  // namespace rodaq::canonical
