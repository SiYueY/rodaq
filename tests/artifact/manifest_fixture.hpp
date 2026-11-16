#pragma once

#include "artifact/manifest.hpp"

#include <cstdint>
#include <string>

namespace rodaq::test {

inline artifact::ManifestIdentity manifest_identity(std::uint64_t segment_id = 7) {
    artifact::ManifestIdentity result;
    result.segment_id = segment_id;
    result.task_id = parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    result.pipeline_id = 3;
    result.instance_id = "21234567-89ab-4cde-8fab-0123456789ab";
    result.segment_sequence = 1;
    result.config_version = "sha256:" + std::string(64, 'c');
    result.effective_config_id = "sha256:" + std::string(64, 'd');
    result.control_generation = 7;
    result.reset_generation_at_commit = 2;
    result.volume_id = artifact::parse_volume_id("11234567-89ab-4cde-8fab-0123456789ab").value();
    result.acquisition = {"rodaq.fake.acquisition", "1.0.0", std::string(64, 'e')};
    result.storage = {"rodaq.fake.storage", "1.0.0", std::string(64, 'f')};
    result.qualified_runtime_profile_id = "behavioral-fake.v1";
    result.format = "fake";
    result.playback_anchor = {42, 10, 1000};
    return result;
}

inline artifact::ManifestDescriptor manifest_descriptor(
    StreamId stream_id = 1, rodaq_descriptor_id_t descriptor_id = 1) {
    artifact::ManifestDescriptor result;
    result.stream_id = stream_id;
    result.descriptor_id = descriptor_id;
    result.data_profile = RODAQ_DATA_PROFILE_GENERIC;
    result.channel_name = "/data";
    result.message_encoding = "json";
    result.schema_digest =
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    return result;
}

}  // namespace rodaq::test
