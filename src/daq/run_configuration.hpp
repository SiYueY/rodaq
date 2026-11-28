#pragma once

#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "composition.hpp"

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

Result<task::RunInputs> resolve_run(
    const RunResources& resources, TaskId task_id, std::string_view config_version,
    ControlGeneration control_generation, const TaskConfig& config);

}  // namespace rodaq::daq
