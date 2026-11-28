#include "record.hpp"

#include <cassert>
#include <limits>

namespace rodaq::pipeline {

storage::Record Record::view() const noexcept {
    assert(descriptor && descriptor->stream_id == stream_id && acceptance_sequence != 0);
    assert(payload && payload.size() <= std::numeric_limits<std::uint32_t>::max());
    storage::Record result{};
    result.header.struct_size = sizeof(result);
    result.stream_id = stream_id;
    result.descriptor_id = descriptor->descriptor_id;
    result.recording_time = recording_time_ns;
    result.publish_time = publish_time_ns;
    result.payload = {
        reinterpret_cast<const std::uint8_t*>(payload.data()),
        static_cast<std::uint32_t>(payload.size())};
    result.has_publish_time = has_publish_time;
    return result;
}

}  // namespace rodaq::pipeline
