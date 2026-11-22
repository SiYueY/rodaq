#pragma once

#include "composition.hpp"

#include <mutex>

#include "../persistence/json.hpp"

namespace rodaq::daq {

struct RunResources {
    CompositionOptions options;
    std::vector<std::shared_ptr<ExecutionLane>> writers;
    // One fixed slice per registered Task for this composition lifetime. The
    // control catalog uses the same max_tasks bound; no Native calls under lock.
    mutable std::mutex writer_ownership;
    mutable std::vector<TaskId> writer_tasks;
    // Serializes actual single-Volume Commit execution, including durable
    // Ledger mutation. It never guards queries, Admission or control state.
    std::mutex commit_execution;
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
