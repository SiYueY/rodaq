#pragma once

#include <rodaq/task/config.hpp>

#include "../pipeline/stream_identity.hpp"

namespace rodaq::task {

struct AssignedConfig {
    TaskConfig config;
    PipelineId pipeline_id_high_water{0};
    StreamId stream_id_high_water{0};
};

// Initial allocation is deterministic in portable array order, including after
// a Create crash. Explicit imported IDs are preserved within the new Task scope.
Result<AssignedConfig> assign_initial_config(
    const TaskConfig& request, const TaskConfigLimits& limits);

// These high-water values come from durable identity authority, not from the
// current Config alone: removed IDs remain unavailable after GC and Reset.
Result<AssignedConfig> assign_config_change(
    const TaskConfig& current, const TaskConfig& request, PipelineId pipeline_id_high_water,
    StreamId stream_id_high_water, const TaskConfigLimits& limits);

Result<pipeline::StreamIdentityProjection> identity_projection(
    const AssignedConfig& assigned, const TaskConfigLimits& limits);

}  // namespace rodaq::task
