#include "stream_table.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq::pipeline {

Result<StreamTable> StreamTable::create(
    std::vector<StreamPreparation> streams, std::size_t max_streams) {
    if (max_streams == 0 || max_streams > 4096 || streams.empty() || streams.size() > max_streams) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::sort(streams.begin(), streams.end(), [](const auto& left, const auto& right) {
        return left.stream_id < right.stream_id;
    });
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const auto& stream = streams[i];
        // Matches the formal Task stream_key grammar, not a backend topic rule.
        if (stream.stream_id == 0 || (i != 0 && streams[i - 1].stream_id == stream.stream_id) ||
            stream.max_record_bytes == 0 ||
            stream.max_record_bytes > std::numeric_limits<std::uint32_t>::max() ||
            stream.stream_key.empty() || stream.stream_key.size() > 128 ||
            !std::all_of(
                stream.stream_key.begin(), stream.stream_key.end(),
                [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; }) ||
            stream.sequences.begin == 0 ||
            stream.sequences.begin >= stream.sequences.end_exclusive ||
            stream.descriptors.begin == 0 ||
            stream.descriptors.begin >= stream.descriptors.end_exclusive) {
            return Status(StatusCode::InvalidArgument, {});
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (streams[j].stream_key == stream.stream_key) {
                return Status(StatusCode::InvalidArgument, {});
            }
        }
    }
    try {
        StreamTable table;
        table.entries_.reserve(streams.size());
        table.bindings_.reserve(streams.size());
        for (auto& stream : streams) {
            auto entry = std::make_unique<Entry>(
                stream.stream_id, std::move(stream.stream_key), stream.max_record_bytes);
            auto sequence_started = entry->admission.sequences.begin_refill(1);
            auto descriptor_started = entry->admission.descriptor_ids.begin_refill(1);
            if (!sequence_started.ok() || !descriptor_started.ok()) {
                return Status(StatusCode::Internal, {});
            }
            auto sequence_ready = entry->admission.sequences.finish_refill(1, stream.sequences);
            auto descriptor_ready =
                entry->admission.descriptor_ids.finish_refill(1, stream.descriptors);
            if (!sequence_ready.ok() || !descriptor_ready.ok()) {
                return Status(StatusCode::Internal, {});
            }
            table.bindings_.push_back(
                {{sizeof(acquisition::StreamBinding)},
                 stream.stream_id,
                 {entry->key.data(), static_cast<std::uint32_t>(entry->key.size())},
                 stream.max_record_bytes});
            table.entries_.push_back(std::move(entry));
        }
        return table;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

StreamAdmission* StreamTable::find(StreamId id) noexcept {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const auto& entry, StreamId target) { return entry->admission.stream_id < target; });
    return found != entries_.end() && (*found)->admission.stream_id == id ? &(*found)->admission
                                                                          : nullptr;
}

}  // namespace rodaq::pipeline
