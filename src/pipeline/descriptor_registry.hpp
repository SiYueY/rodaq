#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <rodaq/result.hpp>

#include "descriptor.hpp"

namespace rodaq::pipeline {

struct DescriptorRegistryOptions {
    std::uint32_t max_descriptors_per_stream{16};
    std::uint32_t max_schema_bytes{1024 * 1024};
    std::uint32_t max_offered_qos_profiles{64};
    // Data bytes are schema + proposal text + typed QoS. Fixed digests/slot/control metadata
    // is bounded separately by configured Stream/descriptor cardinality.
    std::size_t max_owned_data_bytes{16 * 1024 * 1024};
};

struct DescriptorRegistrySnapshot {
    std::size_t live_descriptors{0};
    std::size_t owned_data_bytes{0};
    std::size_t high_water_descriptors{0};
    std::size_t high_water_data_bytes{0};
};

// Immutable Descriptor owners are independent of StreamState/current selection.
// Weak indexing does not keep obsolete descriptors alive; accepted Record and
// Segment owners keep them referenced through actual use and normal settlement.
class DescriptorRegistry {
public:
    static Result<DescriptorRegistry> create(
        std::vector<rodaq_stream_id_t> streams, DescriptorRegistryOptions options = {});
    DescriptorRegistry(const DescriptorRegistry&) = delete;
    DescriptorRegistry& operator=(const DescriptorRegistry&) = delete;
    DescriptorRegistry(DescriptorRegistry&&) noexcept = default;
    DescriptorRegistry& operator=(DescriptorRegistry&&) noexcept = default;

    Result<DescriptorRef> capture(
        const acquisition::DescriptorProposal& proposal,
        rodaq_descriptor_id_t durable_descriptor_id);
    Result<DescriptorRef> lookup(rodaq_stream_id_t stream, rodaq_descriptor_id_t descriptor) const;
    DescriptorRegistrySnapshot snapshot() const;

private:
    explicit DescriptorRegistry(std::shared_ptr<DescriptorRegistryState> state) noexcept;
    std::shared_ptr<DescriptorRegistryState> state_;
};

}  // namespace rodaq::pipeline
