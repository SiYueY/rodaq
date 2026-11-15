#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/plugin/types.hpp>
#include <rodaq/storage/types.hpp>
#include <rodaq/task/types.hpp>

#include "managed_volume.hpp"
#include "sealed_files.hpp"
#include "../time/clock.hpp"

namespace rodaq::artifact {

// Schema bytes remain in the self-decoding Storage file set; Core records its
// own byte digest and all typed Descriptor facts, not a provider-supplied hash.
struct ManifestDescriptor {
    StreamId stream_id{0};
    rodaq_descriptor_id_t descriptor_id{0};
    std::uint64_t accepted_records{0};
    rodaq_data_profile_t data_profile{RODAQ_DATA_PROFILE_INVALID};
    std::string channel_name;
    std::string message_encoding;
    std::string schema_name;
    std::string schema_encoding;
    std::string schema_digest;
    std::uint32_t schema_size{0};
    std::string ros2_type_description_hash;
    std::vector<rodaq_ros2_qos_profile_t> offered_qos;
};

struct StreamCoverage {
    StreamId stream_id{0};
    std::uint64_t accepted{0};
    std::uint64_t written{0};
    std::uint64_t known_failed{0};
    std::uint64_t unknown{0};
    std::uint64_t first_sequence{0};
    std::uint64_t last_sequence{0};
};

struct ManifestEpoch {
    rodaq_clock_epoch_id_t epoch_id{0};
    time::ClockEpochOrigin origin{time::ClockEpochOrigin::Boot};
    std::optional<time::ClockJump> jump;
    std::uint64_t accepted_records{0};
    std::uint64_t first_receive_monotonic{0};
    std::uint64_t last_receive_monotonic{0};
    std::uint64_t max_sample_error_bound_ns{0};
};

struct SegmentManifest {
    std::uint64_t segment_id{0};
    TaskId task_id;
    PipelineId pipeline_id{0};
    std::string instance_id;
    std::uint64_t segment_sequence{0};
    std::string config_version;
    std::string effective_config_id;
    ControlGeneration control_generation{0};
    std::uint64_t reset_generation_at_commit{0};
    VolumeId volume_id;
    plugin::PluginBinaryInfo acquisition;
    plugin::PluginBinaryInfo storage;
    std::string qualified_runtime_profile_id;
    std::vector<FileEvidence> files;
    std::vector<ManifestDescriptor> descriptors;
    std::vector<StreamCoverage> coverage;
    time::PlaybackAnchor playback_anchor;
    std::vector<ManifestEpoch> epochs;
    std::uint64_t first_receive_monotonic{0};
    std::uint64_t last_receive_monotonic{0};
    std::int64_t min_receive_realtime{0};
    std::int64_t max_receive_realtime{0};
    std::uint64_t recording_begin_ns{0};
    std::uint64_t recording_end_ns{0};
};

struct ManifestLimits {
    std::size_t max_bytes{4 * 1024 * 1024};
    std::uint32_t max_files{64};
    std::uint32_t max_descriptors{1024};
    std::uint32_t max_streams{1024};
    std::uint32_t max_epochs{64};
    std::uint32_t max_qos_profiles{256};
};

Result<ManifestDescriptor> capture_manifest_descriptor(
    const storage::StreamDescriptor& descriptor, std::uint64_t accepted_records,
    std::uint32_t max_schema_bytes = 1024 * 1024, std::uint32_t max_qos_profiles = 256);
Result<void> validate_manifest(const SegmentManifest& manifest, const ManifestLimits& limits);
Result<std::string> encode_manifest(const SegmentManifest& manifest, const ManifestLimits& limits);
Result<SegmentManifest> decode_manifest(std::string_view encoded, const ManifestLimits& limits);
Result<std::string> manifest_digest(const SegmentManifest& manifest, const ManifestLimits& limits);

}  // namespace rodaq::artifact
