#include "rodaqmeta.hpp"

#include <limits>

#include <rodaq/storage/validation.hpp>

namespace rodaq::mcap_storage {
namespace {

class Encoder {
public:
    explicit Encoder(std::byte* output) noexcept : next_(output) {}

    void number(std::uint64_t value, unsigned width) noexcept {
        for (unsigned i = 0; i < width; ++i) {
            *next_++ = static_cast<std::byte>(value & 0xff);
            value >>= 8;
        }
    }

    void text(std::string_view value) noexcept {
        for (unsigned char byte : value) {
            *next_++ = static_cast<std::byte>(byte);
        }
    }

private:
    std::byte* next_;
};

class Decoder {
public:
    explicit Decoder(std::string_view bytes) noexcept : bytes_(bytes) {}

    std::uint64_t number(unsigned width) noexcept {
        if (width > bytes_.size()) {
            valid_ = false;
            return 0;
        }
        std::uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i) {
            result |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[i])) << (8 * i);
        }
        bytes_.remove_prefix(width);
        return result;
    }

    bool valid() const noexcept { return valid_; }

private:
    std::string_view bytes_;
    bool valid_{true};
};

std::int64_t signed_value(std::uint64_t bits) noexcept {
    return bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
               ? static_cast<std::int64_t>(bits)
               : -static_cast<std::int64_t>(~bits) - 1;
}

std::uint32_t entry_size(const storage::Record& record) noexcept {
    return static_cast<std::uint32_t>(
        kMetaEntryBaseBytes + (record.has_source_id ? kMetaSourceBytes : 0) +
        (record.has_source_session_id ? 8 : 0) + (record.has_source_sequence ? 12 : 0) +
        record.timestamp_count * kMetaTimestampBytes);
}

}  // namespace

std::array<std::byte, kMetaHeaderBytes> encode_meta_header() noexcept {
    std::array<std::byte, kMetaHeaderBytes> bytes{};
    Encoder out(bytes.data());
    out.text("RDAQMETA");
    out.number(1, 2);
    out.number(0, 2);
    out.number(kMetaHeaderBytes, 4);
    out.number(0, 4);
    out.number(0, 4);
    return bytes;
}

Result<MetaEntry> encode_meta_entry(const storage::Record& record, std::uint64_t ordinal) {
    if (!storage::abi::valid_record(record)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    MetaEntry entry;
    entry.size = entry_size(record);
    Encoder out(entry.bytes.data());
    out.number(entry.size, 4);
    out.number(
        record.has_source_id | (record.has_source_session_id << 1) |
            (record.has_source_sequence << 2),
        4);
    out.number(ordinal, 8);
    out.number(record.stream_id, 8);
    out.number(record.acceptance_sequence, 8);
    out.number(record.descriptor_id, 8);
    out.number(record.system_boot_id, 8);
    out.number(record.clock_epoch_id, 8);
    out.number(record.receive_monotonic, 8);
    out.number(static_cast<std::uint64_t>(record.receive_realtime), 8);
    if (record.has_source_id) {
        out.number(record.source_id.kind, 4);
        out.number(record.source_id.size, 1);
        for (auto byte : record.source_id.data) {
            out.number(byte, 1);
        }
        out.number(0, 3);
    }
    if (record.has_source_session_id) {
        out.number(record.source_session_id, 8);
    }
    if (record.has_source_sequence) {
        out.number(record.source_sequence, 8);
        out.number(record.sequence_scope, 4);
    }
    out.number(record.timestamp_count, 4);
    for (std::uint32_t i = 0; i < record.timestamp_count; ++i) {
        const auto& timestamp = record.timestamps[i];
        out.number(timestamp.role, 4);
        out.number(timestamp.clock_domain, 4);
        out.number(timestamp.clock_id, 8);
        out.number(static_cast<std::uint64_t>(timestamp.value), 8);
        out.number(timestamp.flags, 4);
        out.number(timestamp.reserved, 4);
    }
    out.number(record.delivery_flags, 4);
    return entry;
}

Result<std::array<std::byte, kMetaFooterBytes>> encode_meta_footer(MetaFooter footer) {
    if (footer.bytes_before_footer > std::numeric_limits<std::uint64_t>::max() - kMetaFooterBytes) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    std::array<std::byte, kMetaFooterBytes> bytes{};
    Encoder out(bytes.data());
    out.text(std::string_view("RDAQEND\0", 8));
    out.number(footer.entry_count, 8);
    out.number(footer.bytes_before_footer, 8);
    return bytes;
}

Result<MetaHeader> decode_meta_header(std::string_view bytes) {
    if (bytes.size() < kMetaHeaderBytes || bytes.substr(0, 8) != "RDAQMETA") {
        return Status(StatusCode::DataLoss, {});
    }
    Decoder in(bytes.substr(8));
    const auto major = in.number(2);
    MetaHeader header;
    header.minor = static_cast<std::uint16_t>(in.number(2));
    header.size = static_cast<std::uint32_t>(in.number(4));
    const auto flags = in.number(4);
    const auto reserved = in.number(4);
    if (major != 1 || header.size < kMetaHeaderBytes || header.size > bytes.size() || flags ||
        reserved || (header.minor == 0 && header.size != kMetaHeaderBytes)) {
        return Status(StatusCode::DataLoss, {});
    }
    return header;
}

Result<DecodedMetaEntry> decode_meta_entry(std::string_view bytes, std::uint16_t minor) {
    if (bytes.size() < kMetaEntryBaseBytes) {
        return Status(StatusCode::DataLoss, {});
    }
    Decoder prefix(bytes);
    DecodedMetaEntry entry;
    entry.size = static_cast<std::uint32_t>(prefix.number(4));
    if (entry.size < kMetaEntryBaseBytes || entry.size > bytes.size()) {
        return Status(StatusCode::DataLoss, {});
    }
    Decoder in(bytes.substr(4, entry.size - 4));
    const auto presence = in.number(4);
    if ((presence & ~std::uint64_t{7}) || ((presence & 6) && !(presence & 1))) {
        return Status(StatusCode::DataLoss, {});
    }
    entry.ordinal = in.number(8);
    auto& record = entry.record;
    record.header.struct_size = sizeof(record);
    record.stream_id = in.number(8);
    record.acceptance_sequence = in.number(8);
    record.descriptor_id = in.number(8);
    record.system_boot_id = in.number(8);
    record.clock_epoch_id = in.number(8);
    record.receive_monotonic = in.number(8);
    record.receive_realtime = signed_value(in.number(8));
    record.has_source_id = (presence & 1) != 0;
    record.has_source_session_id = (presence & 2) != 0;
    record.has_source_sequence = (presence & 4) != 0;
    if (record.has_source_id) {
        record.source_id.kind = static_cast<std::uint32_t>(in.number(4));
        record.source_id.size = static_cast<std::uint8_t>(in.number(1));
        for (auto& byte : record.source_id.data) {
            byte = static_cast<std::uint8_t>(in.number(1));
        }
        for (auto& byte : record.source_id.reserved) {
            byte = static_cast<std::uint8_t>(in.number(1));
        }
    }
    if (record.has_source_session_id) {
        record.source_session_id = in.number(8);
    }
    if (record.has_source_sequence) {
        record.source_sequence = in.number(8);
        record.sequence_scope = static_cast<std::uint32_t>(in.number(4));
    }
    record.timestamp_count = static_cast<std::uint32_t>(in.number(4));
    if (record.timestamp_count > RODAQ_STORAGE_TIMESTAMP_CAPACITY) {
        return Status(StatusCode::DataLoss, {});
    }
    for (std::uint32_t i = 0; i < record.timestamp_count; ++i) {
        auto& timestamp = record.timestamps[i];
        timestamp.role = static_cast<std::uint32_t>(in.number(4));
        timestamp.clock_domain = static_cast<std::uint32_t>(in.number(4));
        timestamp.clock_id = in.number(8);
        timestamp.value = signed_value(in.number(8));
        timestamp.flags = static_cast<std::uint32_t>(in.number(4));
        timestamp.reserved = static_cast<std::uint32_t>(in.number(4));
    }
    record.delivery_flags = static_cast<std::uint32_t>(in.number(4));
    const auto known_size = entry_size(record);
    if (!in.valid() || !storage::abi::valid_record(record) || entry.size < known_size ||
        (minor == 0 && entry.size != known_size)) {
        return Status(StatusCode::DataLoss, {});
    }
    return entry;
}

Result<MetaFooter> decode_meta_footer(std::string_view bytes) {
    if (bytes.size() != kMetaFooterBytes ||
        bytes.substr(0, 8) != std::string_view("RDAQEND\0", 8)) {
        return Status(StatusCode::DataLoss, {});
    }
    Decoder in(bytes.substr(8));
    MetaFooter footer{in.number(8), in.number(8)};
    if (footer.bytes_before_footer > std::numeric_limits<std::uint64_t>::max() - kMetaFooterBytes) {
        return Status(StatusCode::DataLoss, {});
    }
    return footer;
}

}  // namespace rodaq::mcap_storage
