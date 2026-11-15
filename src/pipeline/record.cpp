#include "record.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace rodaq::pipeline {

storage::Record Record::view() const noexcept {
    assert(descriptor && descriptor->stream_id == stream_id && acceptance_sequence != 0);
    assert(payload && payload.size() <= std::numeric_limits<std::uint32_t>::max());
    assert(timestamp_count <= timestamps.size());
    storage::Record result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = stream_id;
    result.descriptor_id = descriptor->descriptor_id;
    result.acceptance_sequence = acceptance_sequence;
    result.system_boot_id = received.boot_id;
    result.clock_epoch_id = received.epoch_id;
    result.receive_monotonic = received.sample.monotonic_ns;
    result.receive_realtime = received.sample.realtime_ns;
    result.source_id = source_id;
    result.source_session_id = source_session_id;
    result.source_sequence = source_sequence;
    result.payload = {
        reinterpret_cast<const std::uint8_t*>(payload.data()),
        static_cast<std::uint32_t>(payload.size())};
    std::copy_n(timestamps.begin(), timestamp_count, result.timestamps);
    result.timestamp_count = timestamp_count;
    result.sequence_scope = sequence_scope;
    result.delivery_flags = delivery_flags;
    result.has_source_id = has_source_id;
    result.has_source_session_id = has_source_session_id;
    result.has_source_sequence = has_source_sequence;
    return result;
}

}  // namespace rodaq::pipeline
