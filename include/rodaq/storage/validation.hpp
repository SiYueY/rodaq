#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string_view>

#include <rodaq/canonical/validation.hpp>
#include <rodaq/plugin/abi.hpp>
#include <rodaq/storage/types.hpp>

namespace rodaq::storage::abi {

inline bool valid_prepare(const PrepareConfig* config) noexcept {
    return plugin::abi::readable(config) && config->max_record_bytes != 0 &&
           config->max_record_bytes <= UINT32_MAX && config->max_batch_bytes != 0 &&
           config->max_batch_records != 0 && config->max_descriptors_per_segment != 0 &&
           config->max_segment_records != 0 && config->max_segment_input_bytes != 0 &&
           config->max_segment_physical_bytes != 0 && config->max_segment_file_count != 0 &&
           config->max_relative_path_bytes != 0 && config->max_relative_path_bytes < UINT32_MAX &&
           config->system_boot_id != 0;
}

inline bool valid_capacity(const CapacityModel& model) noexcept {
    return model.header.struct_size >= sizeof(model) && model.payload_expansion_denominator != 0 &&
           model.descriptor_expansion_denominator != 0;
}

inline bool valid_record(const Record& record) noexcept {
    if (record.header.struct_size != sizeof(record) || record.stream_id == 0 ||
        record.descriptor_id == 0 || record.acceptance_sequence == 0 ||
        record.system_boot_id == 0 || record.clock_epoch_id == 0 ||
        (record.payload.size != 0 && record.payload.data == nullptr) ||
        record.timestamp_count > RODAQ_STORAGE_TIMESTAMP_CAPACITY ||
        (record.delivery_flags & ~RODAQ_RECORD_DELIVERY_RETAINED) != 0 ||
        !canonical::valid_source_facts(
            record.source_id, record.source_session_id, record.source_sequence,
            record.sequence_scope, record.has_source_id, record.has_source_session_id,
            record.has_source_sequence) ||
        !std::all_of(std::begin(record.reserved), std::end(record.reserved), [](auto byte) {
            return byte == 0;
        })) {
        return false;
    }
    for (std::uint32_t i = 0; i < RODAQ_STORAGE_TIMESTAMP_CAPACITY; ++i) {
        if (!canonical::valid_timestamp(record.timestamps[i], i < record.timestamp_count)) {
            return false;
        }
    }
    return true;
}

inline bool valid_batch(Batch batch) noexcept {
    if (batch.count == 0 || batch.records == nullptr ||
        reinterpret_cast<std::uintptr_t>(batch.records) % alignof(Record) != 0) {
        return false;
    }
    for (std::uint32_t i = 0; i < batch.count; ++i) {
        if (!valid_record(batch.records[i])) {
            return false;
        }
    }
    return true;
}

inline bool valid_outcome(
    rodaq_status_code_t code, const BatchResult& result, std::uint32_t count) noexcept {
    if (!plugin::abi::known_status(code) || result.header.struct_size < sizeof(result) ||
        result.confirmed_prefix > count) {
        return false;
    }
    if (result.confirmed_prefix == count) {
        return result.tail_state == RODAQ_STORAGE_TAIL_NONE;
    }
    return code != RODAQ_STATUS_OK &&
           (result.tail_state == RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN ||
            result.tail_state == RODAQ_STORAGE_TAIL_UNKNOWN);
}

inline bool valid_descriptor(const StreamDescriptor* descriptor) noexcept {
    return plugin::abi::readable(descriptor) && descriptor->descriptor_id != 0 &&
           descriptor->stream_id != 0 && descriptor->reserved0 == 0 && descriptor->reserved1 == 0 &&
           canonical::valid_descriptor_fields(*descriptor);
}

inline bool valid_segment(const SegmentSpec* segment) noexcept {
    return plugin::abi::readable(segment) && segment->segment_id != 0 &&
           plugin::abi::valid_string(segment->staging_directory) &&
           segment->staging_directory.size != 0 &&
           std::memchr(segment->staging_directory.data, 0, segment->staging_directory.size) ==
               nullptr;
}

inline bool valid_usage(const SegmentUsage& usage) noexcept {
    return usage.header.struct_size >= sizeof(usage) && usage.reserved0 == 0;
}

inline bool valid_relative_path(std::string_view path) noexcept {
    return !path.empty() && path != "." && path != ".." &&
           path.find_first_of("/\\") == std::string_view::npos &&
           path.find('\0') == std::string_view::npos &&
           plugin::abi::utf8_prefix(path, path.size()) == path.size();
}

inline bool valid_sealed_files(const SegmentSealResult& result) noexcept {
    if (result.files.empty()) {
        return false;
    }
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < result.files.size(); ++i) {
        const auto& file = result.files[i];
        if (!valid_relative_path(file.relative_path) ||
            file.physical_size > std::numeric_limits<std::uint64_t>::max() - total) {
            return false;
        }
        total += file.physical_size;
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (result.files[previous].relative_path == file.relative_path) {
                return false;
            }
        }
    }
    return total == result.total_physical_bytes;
}

}  // namespace rodaq::storage::abi
