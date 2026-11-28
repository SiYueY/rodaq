#include <rodaq/daq/runtime.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "composition.hpp"
#include "startup.hpp"

namespace rodaq {

struct DaqRuntime::State {
    std::unique_ptr<daq::Composition> composition;
};

DaqRuntime::DaqRuntime(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

DaqRuntime::~DaqRuntime() noexcept = default;

Result<std::unique_ptr<DaqRuntime>> DaqRuntime::open(DaqOpenOptions options) {
    if (options.profiles.empty() || options.allowed_backends.empty() ||
        options.volume.name.empty() ||
        options.volume.space_sample_max_age <= std::chrono::nanoseconds::zero() ||
        options.volume.recovery_stable_window <= std::chrono::nanoseconds::zero() ||
        options.system_snapshot_max_age <= std::chrono::nanoseconds::zero()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        daq::StartupOptions startup;
        startup.state_root = std::move(options.state_root);
        startup.volume_root = std::move(options.volume_root);
        startup.mode = options.mode == DaqOpenMode::Initialize ? daq::DeploymentOpenMode::Initialize
                                                               : daq::DeploymentOpenMode::Existing;
        startup.plugins.search_paths = std::move(options.plugin_search_paths);
        startup.plugins.max_loaded_plugins = options.max_loaded_plugins;
        startup.plugins.max_instances = options.max_plugin_instances;
        startup.plugins.max_config_size = options.max_plugin_config_bytes;
        startup.plugins.shutdown_timeout = options.plugin_shutdown_timeout;

        startup.capacity.max_managed_bytes = options.volume.max_managed_bytes;
        startup.capacity.max_commit_bytes = options.volume.max_commit_bytes;
        startup.capacity.stop_free_bytes = options.volume.stop_free_bytes;
        startup.capacity.low_free_bytes = options.volume.low_free_bytes;
        startup.capacity.recovery_free_bytes = options.volume.recovery_free_bytes;
        startup.capacity.space_sample_max_age_ns =
            static_cast<std::uint64_t>(options.volume.space_sample_max_age.count());
        startup.capacity.recovery_stable_ns =
            static_cast<std::uint64_t>(options.volume.recovery_stable_window.count());
        startup.capacity.max_segments = options.volume.max_segments;
        startup.capacity.max_commit_slots = options.volume.max_commit_slots;
        startup.capacity.max_artifact_slots = options.volume.max_artifacts_per_volume;

        auto& settings = startup.settings;
        settings.deployment_generation = options.deployment_generation;
        settings.qualified_runtime_profile_id = std::move(options.qualified_runtime_profile_id);
        settings.output_root = std::move(options.volume.name);
        settings.max_tasks = options.max_tasks;
        settings.max_artifacts_per_volume = options.volume.max_artifacts_per_volume;
        settings.durability = options.volume.durability;
        settings.native_timeout = options.native_timeout;
        settings.system_snapshot_max_age = options.system_snapshot_max_age;
        settings.tasks.config.max_pipelines = options.max_pipelines_per_task;

        settings.profiles.reserve(options.profiles.size());
        for (const auto& profile : options.profiles) {
            if (profile.max_records == 0 ||
                profile.max_records > std::numeric_limits<std::uint32_t>::max()) {
                return Status(StatusCode::InvalidArgument, {});
            }
            daq::ResourceProfile target;
            target.name = profile.name;
            target.instance.records = static_cast<std::uint32_t>(profile.max_records);
            target.instance.logical_bytes = profile.max_logical_bytes;
            target.instance.payloads.physical_bytes = profile.payload_pool_bytes;
            target.instance.payloads.page_bytes = profile.payload_min_chunk_bytes;
            target.instance.payloads.max_payload_bytes = profile.max_payload_bytes;
            target.instance.payloads.max_buffers = profile.payload_max_chunks;
            target.instance.queue_capacity = profile.queue_capacity;
            target.instance.callback_concurrency = profile.callback_concurrency;
            target.instance.max_descriptor_metadata_bytes = profile.max_descriptor_metadata_bytes;
            target.storage = {};
            target.storage.header = {sizeof(storage::PrepareConfig)};
            target.storage.max_record_bytes = profile.max_payload_bytes;
            target.storage.max_batch_bytes = profile.max_batch_input_bytes;
            target.storage.max_batch_records = profile.max_batch_records;
            target.storage.max_descriptors_per_segment = profile.max_descriptors;
            target.storage.max_segment_records = profile.max_segment_records;
            target.storage.max_segment_input_bytes = profile.max_segment_input_bytes;
            target.storage.max_segment_physical_bytes = profile.max_segment_physical_bytes;
            target.storage.max_segment_file_count = profile.max_sealed_files;
            target.storage.max_relative_path_bytes = 256;
            target.segment_reservation_bytes = profile.segment_reservation_bytes;
            target.manifest.max_bytes = profile.max_manifest_bytes;
            target.manifest.max_descriptor_metadata_bytes = profile.max_descriptor_metadata_bytes;
            target.max_prepared_descriptors = profile.max_prepared_descriptors;
            settings.profiles.push_back(std::move(target));
        }

        for (const auto& backend : options.allowed_backends) {
            const auto role = backend.role == BackendRole::Acquisition
                                  ? task::AdmissionRole::Acquisition
                                  : task::AdmissionRole::Storage;
            settings.allowed_plugins.push_back({backend.plugin_id, backend.plugin_version, role});
            if (backend.role == BackendRole::Storage) {
                settings.storage.push_back(
                    {backend.plugin_id, backend.plugin_version, backend.storage_format});
            }
            settings.backend_qualifications.push_back(
                {backend.plugin_id, backend.plugin_version, backend.role, backend.support_status,
                 backend.qualification_status});
        }

        auto composed = daq::Composition::open(std::move(startup));
        if (!composed.ok()) {
            return composed.status();
        }
        auto state = std::make_unique<State>();
        state->composition = std::move(composed.value());
        return std::unique_ptr<DaqRuntime>(new DaqRuntime(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

TaskService& DaqRuntime::tasks() noexcept { return state_->composition->tasks(); }

ArtifactService& DaqRuntime::artifacts() noexcept { return state_->composition->artifacts(); }

SystemService& DaqRuntime::system() noexcept { return state_->composition->system(); }

Result<void> DaqRuntime::observe() { return state_->composition->observe(); }

Result<void> DaqRuntime::shutdown(std::chrono::steady_clock::time_point deadline) {
    return state_->composition->shutdown(deadline);
}

}  // namespace rodaq
