#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <rodaq/result.hpp>
#include <rodaq/storage/types.hpp>

namespace rodaq::mcap_storage {

constexpr std::size_t kMetaHeaderBytes = 8 + 2 + 2 + 4 + 4 + 4;
constexpr std::size_t kMetaFooterBytes = 8 + 8 + 8;
constexpr std::size_t kMetaEntryBaseBytes = 4 + 4 + 8 + 7 * 8 + 4 + 4;
constexpr std::size_t kMetaSourceBytes = 4 + 1 + 24 + 3;
constexpr std::size_t kMetaTimestampBytes = 4 + 4 + 8 + 8 + 4 + 4;
constexpr std::size_t kMetaMaxEntryBytes = kMetaEntryBaseBytes + kMetaSourceBytes + 8 + 8 + 4 +
                                           RODAQ_STORAGE_TIMESTAMP_CAPACITY * kMetaTimestampBytes;
static_assert(RODAQ_SOURCE_ID_DATA_CAPACITY == 24);
static_assert(kMetaHeaderBytes == 24 && kMetaFooterBytes == 24 && kMetaMaxEntryBytes == 324);

struct MetaEntry {
    std::array<std::byte, kMetaMaxEntryBytes> bytes{};
    std::uint32_t size{0};
};

struct MetaHeader {
    std::uint16_t minor{0};
    std::uint32_t size{0};
};

struct DecodedMetaEntry {
    std::uint64_t ordinal{0};
    storage::Record record{};  // Metadata only; payload is not stored here.
    std::uint32_t size{0};
};

struct MetaFooter {
    std::uint64_t entry_count{0};
    std::uint64_t bytes_before_footer{0};
};

std::array<std::byte, kMetaHeaderBytes> encode_meta_header() noexcept;
Result<MetaEntry> encode_meta_entry(const storage::Record& record, std::uint64_t ordinal);
Result<std::array<std::byte, kMetaFooterBytes>> encode_meta_footer(MetaFooter footer);
Result<MetaHeader> decode_meta_header(std::string_view bytes);
Result<DecodedMetaEntry> decode_meta_entry(std::string_view bytes, std::uint16_t minor);
Result<MetaFooter> decode_meta_footer(std::string_view bytes);

}  // namespace rodaq::mcap_storage
