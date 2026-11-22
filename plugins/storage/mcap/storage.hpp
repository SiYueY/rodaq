#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <rodaq/plugin/deadline.hpp>
#include <rodaq/storage/types.hpp>

#include "checked_sink.hpp"

namespace rodaq::mcap_storage {

storage::CapacityModel capacity_model(std::string_view recorder_distro) noexcept;

// One actual Storage Writer lane owns all methods and file mutation. There are
// no background threads, business queues, publication or filesystem durability.
class StorageBackend {
public:
    explicit StorageBackend(CheckedSink::Write mcap_write = {}, CheckedSink::Write meta_write = {});
    StorageBackend(const StorageBackend&) = delete;
    StorageBackend& operator=(const StorageBackend&) = delete;
    ~StorageBackend() noexcept;

    Result<void> configure(std::string_view input);
    Result<storage::CapacityModel> prepare(const storage::PrepareConfig& config, plugin::Deadline);
    Result<void> prepare_descriptor(const storage::StreamDescriptor& descriptor, plugin::Deadline);
    Result<void> open_segment(const storage::SegmentSpec& segment, plugin::Deadline);
    storage::WriteOutcome write_batch(storage::Batch batch, plugin::Deadline);
    Result<storage::SegmentUsage> segment_usage() const;
    Result<storage::SegmentSealResult> seal_segment(plugin::Deadline);
    Result<void> quiesce(plugin::Deadline);

private:
    enum class Phase { New, Configured, Prepared, Open, Sealed, Failed, Stopped };
    struct Descriptor;
    struct SchemaLess {
        bool operator()(const Descriptor* left, const Descriptor* right) const;
    };
    using DescriptorKey = std::pair<rodaq_stream_id_t, rodaq_descriptor_id_t>;

    Result<std::uint64_t> playback_time(const storage::Record& record) const;
    Result<void> materialize(Descriptor& descriptor);
    void fail(StatusCode error) noexcept;

    CheckedSink::Write mcap_write_;
    CheckedSink::Write meta_write_;
    std::unique_ptr<CheckedSink> mcap_;
    std::unique_ptr<CheckedSink> meta_;
    std::unique_ptr<mcap::McapWriter> writer_;
    std::map<DescriptorKey, Descriptor> descriptors_;
    // Keys borrow immutable Schema templates from stable descriptor map nodes.
    // This index is cleared per Segment and destroyed before those owners.
    std::map<const Descriptor*, mcap::SchemaId, SchemaLess> schemas_;
    storage::PrepareConfig limits_{};
    storage::CapacityModel model_{};
    Phase phase_{Phase::New};
    StatusCode failure_{StatusCode::Ok};
    std::uint64_t catalog_bytes_{0};
    std::uint64_t segment_id_{0};
    std::uint64_t paired_records_{0};
    std::uint64_t input_bytes_{0};
    std::uint32_t channel_count_{0};
    std::string mcap_name_;
    std::string meta_name_;
};

}  // namespace rodaq::mcap_storage
