#ifndef RODAQ_CANONICAL_MODEL_HPP_
#define RODAQ_CANONICAL_MODEL_HPP_

#include <rodaq/plugin/abi/canonical.h>
#include <canonical/handle.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <vector>

namespace rodaq {

using ArenaString = std::pmr::string;
using ArenaBytes = std::pmr::vector<std::uint8_t>;

struct SourceEntry {
    ArenaBytes source_id;
    std::size_t plugin_index = 0;
    std::unique_ptr<std::atomic<std::uint32_t>> availability;
};

struct ClockEntry {
    ArenaBytes native_id;
    std::uint64_t clock_id = 0;
    std::uint64_t resolution_numerator = 0;
    std::uint64_t resolution_denominator = 0;
    ArenaString origin;
    std::size_t plugin_index = 0;
};

struct SchemaEntry {
    std::uint64_t schema_id = 0;
    ArenaString format;
    bool type_identifier_present = false;
    ArenaString type_identifier;
    ArenaBytes representation;
};

struct TimestampKeyRecord {
    ArenaString role;
    ClockHandle clock;
};

struct TimestampKeySetEntry {
    std::uint64_t timestamp_key_set_id = 0;
    std::pmr::vector<TimestampKeyRecord> keys;
    std::pmr::vector<rodaq_timestamp_key> abi_keys;
};

struct StreamEntry {
    std::uint64_t stream_id = 0;
    std::size_t plugin_index = 0;
    ArenaBytes native_id;
    std::unique_ptr<std::atomic<std::uint32_t>> availability;
    std::unique_ptr<std::atomic<std::uint64_t>> current_binding;
    std::unique_ptr<std::mutex> admission_mutex;
    std::unique_ptr<std::mutex> binding_transition_mutex;
    std::unique_ptr<std::condition_variable> binding_transition_condition;
    bool binding_transition_active = false;
    std::uint64_t binding_transition_token = 0;
};

enum class DescriptorLifecycle : std::uint32_t {
    Published = 1,
    ReadinessPending = 2,
    Ready = 3,
    BindingPrepared = 4,
    Active = 5,
    Unsupported = 6,
    Failed = 7,
    Superseded = 8,
};

struct StreamDescriptorEntry {
    std::uint64_t stream_descriptor_id = 0;
    StreamHandle stream;
    bool schema_present = false;
    SchemaHandle schema;
    std::uint64_t schema_id = 0;
    ArenaString name;
    ArenaString payload_encoding;
    std::uint64_t timestamp_key_set_id = 0;
    std::pmr::vector<rodaq_timestamp_key> canonical_timestamp_keys;
    std::pmr::vector<rodaq_timestamp_key> transport_timestamp_keys;
    std::size_t plugin_index = 0;
    DescriptorLifecycle lifecycle = DescriptorLifecycle::Published;
    bool reconciliation_running = false;
    bool reconciliation_retry_requested = false;
};

}  // namespace rodaq

#endif  // RODAQ_CANONICAL_MODEL_HPP_
