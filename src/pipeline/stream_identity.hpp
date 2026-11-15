#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/task/types.hpp>

namespace rodaq::pipeline {

struct StreamIdentity {
    StreamId stream_id{0};
    std::string stream_key;
    std::uint64_t reserved_end_exclusive{1};
    std::uint64_t reserved_descriptor_end_exclusive{1};
    bool retired{false};
    std::optional<std::string> retired_at_config_version;
};

struct StreamBinding {
    StreamId stream_id{0};
    std::string stream_key;
};

struct StreamIdentityProjection {
    std::string config_version;
    PipelineId pipeline_id_high_water{0};
    StreamId stream_id_high_water{0};
    std::vector<StreamBinding> streams;
};

struct StreamIdentityState {
    TaskId task_id;
    std::uint64_t record_revision{1};
    PipelineId pipeline_id_high_water{0};
    StreamId stream_id_high_water{0};
    // This is a projection checkpoint, not a second active Config authority.
    // Boot reconciliation selects the projection from Task Control authority.
    std::string observed_config_version;
    std::vector<StreamIdentity> streams;
};

struct IdentityRange {
    std::uint64_t begin{0};
    std::uint64_t end_exclusive{0};
};

Result<void> validate_identity_projection(
    const StreamIdentityProjection& projection, std::size_t max_identities);

Result<void> validate_stream_identity(const StreamIdentityState& state, std::size_t max_identities);
Result<std::string> encode_stream_identity(
    const StreamIdentityState& state, std::size_t max_identities);
Result<StreamIdentityState> decode_stream_identity(
    std::string_view bytes, std::size_t max_identities);
std::size_t stream_identity_max_bytes(std::size_t max_identities) noexcept;

}  // namespace rodaq::pipeline
