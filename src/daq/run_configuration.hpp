#pragma once

#include "composition.hpp"

#include <mutex>
#include <optional>

#include "../persistence/json.hpp"

namespace rodaq::daq {

struct RunResources {
    CompositionOptions options;
    std::vector<std::shared_ptr<ExecutionLane>> writers;
    // Fixed max_tasks slot table; each slot maps to a max_pipelines writer slice
    // and is released on Retire only after actual writer work is proven idle.
    mutable std::mutex writer_ownership;
    mutable std::vector<std::optional<TaskId>> writer_tasks;
};

void release_writer_slot(const RunResources& resources, TaskId task_id) noexcept;

artifact::ManifestLimits catalog_manifest_limits(const DeploymentSettings& settings);

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
