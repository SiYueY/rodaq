#ifndef RODAQ_CANONICAL_CANONICAL_STATE_HPP_
#define RODAQ_CANONICAL_CANONICAL_STATE_HPP_

#include "canonical/model.hpp"
#include "canonical/registry.hpp"

#include <rodaq/daq_config.hpp>
#include <rodaq/plugin/abi/acquisition.h>

#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

namespace rodaq {

class CanonicalState final {
public:
    CanonicalState(
        const DAQConfig& config, std::uint64_t activation_generation,
        std::pmr::memory_resource* arena);

    CanonicalState(const CanonicalState&) = delete;
    CanonicalState& operator=(const CanonicalState&) = delete;
    CanonicalState(CanonicalState&&) = delete;
    CanonicalState& operator=(CanonicalState&&) = delete;

    std::pmr::memory_resource* arena() noexcept { return arena_; }

    Registry<SourceEntry, SourceHandle>& sources() noexcept { return sources_; }
    const Registry<SourceEntry, SourceHandle>& sources() const noexcept { return sources_; }
    Registry<ClockEntry, ClockHandle>& clocks() noexcept { return clocks_; }
    const Registry<ClockEntry, ClockHandle>& clocks() const noexcept { return clocks_; }
    Registry<SchemaEntry, SchemaHandle>& schemas() noexcept { return schemas_; }
    const Registry<SchemaEntry, SchemaHandle>& schemas() const noexcept { return schemas_; }
    Registry<TimestampKeySetEntry, TimestampKeySetHandle>& timestamp_key_sets() noexcept {
        return timestamp_key_sets_;
    }
    const Registry<TimestampKeySetEntry, TimestampKeySetHandle>& timestamp_key_sets()
        const noexcept {
        return timestamp_key_sets_;
    }
    Registry<StreamEntry, StreamHandle>& streams() noexcept { return streams_; }
    const Registry<StreamEntry, StreamHandle>& streams() const noexcept { return streams_; }
    Registry<StreamDescriptorEntry, StreamDescriptorHandle>& descriptors() noexcept {
        return descriptors_;
    }
    const Registry<StreamDescriptorEntry, StreamDescriptorHandle>& descriptors() const noexcept {
        return descriptors_;
    }

    std::vector<SourceHandle>& source_handles() noexcept { return source_handles_; }
    const std::vector<SourceHandle>& source_handles() const noexcept { return source_handles_; }
    std::vector<ClockHandle>& clock_handles() noexcept { return clock_handles_; }
    const std::vector<ClockHandle>& clock_handles() const noexcept { return clock_handles_; }
    std::vector<SchemaHandle>& schema_handles() noexcept { return schema_handles_; }
    const std::vector<SchemaHandle>& schema_handles() const noexcept { return schema_handles_; }
    std::vector<TimestampKeySetHandle>& timestamp_key_set_handles() noexcept {
        return timestamp_key_set_handles_;
    }
    const std::vector<TimestampKeySetHandle>& timestamp_key_set_handles() const noexcept {
        return timestamp_key_set_handles_;
    }
    std::vector<StreamHandle>& stream_handles() noexcept { return stream_handles_; }
    const std::vector<StreamHandle>& stream_handles() const noexcept { return stream_handles_; }
    std::vector<StreamDescriptorHandle>& descriptor_handles() noexcept {
        return descriptor_handles_;
    }
    const std::vector<StreamDescriptorHandle>& descriptor_handles() const noexcept {
        return descriptor_handles_;
    }

    std::uint64_t allocate_clock_id() noexcept { return next_clock_id_++; }
    std::uint64_t allocate_schema_id() noexcept { return next_schema_id_++; }
    std::uint64_t allocate_timestamp_key_set_id() noexcept { return next_timestamp_key_set_id_++; }
    std::uint64_t allocate_stream_id() noexcept { return next_stream_id_++; }
    std::uint64_t allocate_descriptor_id() noexcept { return next_descriptor_id_++; }

    Result<SourceHandle> register_source(
        std::size_t plugin_index, const std::uint8_t* source_id, std::size_t source_id_size);
    Result<ClockHandle> register_clock(
        std::size_t plugin_index, const rodaq_clock_registration& request);
    Result<SchemaHandle> register_schema(const rodaq_schema_registration& request);
    Result<StreamHandle> register_stream(
        std::size_t plugin_index, const std::uint8_t* native_id, std::size_t native_id_size);

    Result<void> get_source(SourceHandle handle, rodaq_source_info& info) const;
    Result<void> get_clock(ClockHandle handle, rodaq_clock_info& info) const;
    Result<void> get_schema(SchemaHandle handle, rodaq_schema_info& info) const;
    Result<void> get_timestamp_key_set(std::uint64_t id, rodaq_timestamp_key_set_info& info) const;
    Result<void> get_stream(StreamHandle handle, rodaq_stream_info& info) const;
    Result<void> get_stream_descriptor(
        StreamDescriptorHandle handle, rodaq_stream_descriptor_info& info) const;

    Result<void> report_source_availability(
        std::size_t plugin_index, SourceHandle source, rodaq_availability_state availability);
    Result<void> report_stream_availability(
        std::size_t plugin_index, StreamHandle stream, rodaq_availability_state availability);

private:
    std::pmr::memory_resource* arena_;
    Registry<SourceEntry, SourceHandle> sources_;
    Registry<ClockEntry, ClockHandle> clocks_;
    Registry<SchemaEntry, SchemaHandle> schemas_;
    Registry<TimestampKeySetEntry, TimestampKeySetHandle> timestamp_key_sets_;
    Registry<StreamEntry, StreamHandle> streams_;
    Registry<StreamDescriptorEntry, StreamDescriptorHandle> descriptors_;

    std::vector<SourceHandle> source_handles_;
    std::vector<ClockHandle> clock_handles_;
    std::vector<SchemaHandle> schema_handles_;
    std::vector<TimestampKeySetHandle> timestamp_key_set_handles_;
    std::vector<StreamHandle> stream_handles_;
    std::vector<StreamDescriptorHandle> descriptor_handles_;

    std::uint64_t next_clock_id_ = 1;
    std::uint64_t next_schema_id_ = 1;
    std::uint64_t next_timestamp_key_set_id_ = 1;
    std::uint64_t next_stream_id_ = 1;
    std::uint64_t next_descriptor_id_ = 1;
};

}  // namespace rodaq

#endif  // RODAQ_CANONICAL_CANONICAL_STATE_HPP_
