#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/plugin/manager.hpp>
#include <rodaq/task/task_service.hpp>

#include "../artifact/managed_volume.hpp"
#include "../execution_lane.hpp"
#include "../pipeline/pipeline_controller.hpp"
#include "../pipeline/recording_instance.hpp"
#include "task_store.hpp"

namespace rodaq::task {

enum class AdmissionRole { Acquisition, Storage };

struct AllowedPlugin {
    std::string plugin_id;
    std::string plugin_version;
    AdmissionRole role{AdmissionRole::Acquisition};
};

struct ResourceProfileAdmission {
    std::string name;
    std::uint64_t max_segment_physical_bytes{0};
};

struct RunPipeline {
    std::shared_ptr<pipeline::PipelineController> controller;
    bool required{true};
    std::optional<StatusCode> prepare_status;
    bool active{false};
};

struct RunInputs {
    std::vector<RunPipeline> pipelines;
};

struct ArtifactOperations {
    artifact::VolumeId volume_id;
    std::function<Result<std::vector<ArtifactId>>(TaskId)> reset_targets;
    std::function<Result<void>(ArtifactId, std::chrono::steady_clock::time_point)> delete_for_reset;
    std::function<Result<bool>(TaskId)> retire_eligible;
};

struct RunExecutionOptions {
    pipeline::ReceiveClock sample{&time::sample_clocks};
    std::shared_ptr<ExecutionLane> preparation_lane;
    std::shared_ptr<ExecutionLane> cleanup_lane;
    std::shared_ptr<ExecutionLane> pipeline_cleanup_lane;
    std::chrono::nanoseconds native_timeout{std::chrono::seconds{5}};
    // Composition resolves frozen Deployment/profile/Volume facts and returns
    // closed Core PipelineControllers (no Native creation/prepare/open here).
    std::function<Result<RunInputs>(
        TaskId, std::string_view config_version, ControlGeneration, const TaskConfig&)>
        resolve;
    // Decoupled bounded Commit handoff with post-commit acknowledgement hook.
    std::function<Result<void>(std::optional<artifact::SealedSegment>&, std::function<void()>)>
        commit_with_ack;
    std::function<Result<void>(artifact::SealedSegment&)> abort_sealed;
    std::function<Result<std::uint64_t>()> allocate_segment_id;
    // Releases per-Task Writer lane slices on Retire after settlement.
    std::function<void(TaskId)> release_task_resources;
    ArtifactOperations artifacts;
};

struct ServiceOptions {
    std::filesystem::path root;
    std::shared_ptr<plugin::PluginManager> plugin_manager;
    std::shared_ptr<ExecutionLane> create_lane;
    std::shared_ptr<ExecutionLane> control_lane;
    std::shared_ptr<const RunExecutionOptions> execution;
    std::vector<AllowedPlugin> allowed_plugins;
    std::vector<ResourceProfileAdmission> resource_profiles;
    std::vector<std::string> output_roots;
    std::size_t max_tasks{8};
    std::size_t max_requests{32};
    TaskStorePolicy store;
    std::chrono::nanoseconds request_wait{std::chrono::seconds{30}};
    std::chrono::nanoseconds runtime_snapshot_max_age{std::chrono::seconds{2}};
    persistence::FileFaultHook file_faults;
};

}  // namespace rodaq::task
