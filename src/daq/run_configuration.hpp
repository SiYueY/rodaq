#pragma once

#include "composition.hpp"

#include "../persistence/json.hpp"

namespace rodaq::daq {

struct RunResources {
    CompositionOptions options;
    std::vector<std::shared_ptr<ExecutionLane>> writers;
};

// Identity excludes volatile Instance/Operation/Segment IDs and clock readings.
// All exact quantities use decimal strings before entering the JCS pipeline.
Result<persistence::Json> effective_configuration(
    const CompositionOptions& deployment, const task::TaskControlRecord& control,
    const PipelineConfig& pipeline, const ResourceProfile& profile,
    const plugin::PluginBinaryInfo& acquisition, const plugin::PluginBinaryInfo& storage);
Result<task::RunInputs> resolve_run(
    const RunResources& resources, const task::TaskControlRecord& control, const TaskConfig& config,
    pipeline::StreamIdentityStore& identities);

}  // namespace rodaq::daq
