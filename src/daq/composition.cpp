#include "composition.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "../artifact/artifact_catalog.hpp"
#include "../artifact/startup_scan.hpp"
#include "../system/system_service.hpp"
#include "run_configuration.hpp"
#include "startup.hpp"

#ifndef RODAQ_APPLICATION_VERSION
#define RODAQ_APPLICATION_VERSION "0.1.0"
#endif

namespace rodaq::daq {
namespace {

BackendRole to_backend_role(task::AdmissionRole role) noexcept {
    return role == task::AdmissionRole::Acquisition ? BackendRole::Acquisition
                                                    : BackendRole::Storage;
}

VolumePressure to_volume_pressure(
    const artifact::VolumeSpaceSnapshot& space, const artifact::VolumeCapacitySnapshot& cap,
    const artifact::CatalogSnapshot& cat, bool force_protected) noexcept {
    if (force_protected || space.protection != StatusCode::Ok || cap.protection != StatusCode::Ok ||
        cat.protection != StatusCode::Ok ||
        space.condition == artifact::VolumeSpaceCondition::Unknown) {
        return VolumePressure::Protected;
    }
    if (space.condition == artifact::VolumeSpaceCondition::Stopped ||
        space.condition == artifact::VolumeSpaceCondition::Recovering) {
        return VolumePressure::Critical;
    }
    if (space.condition == artifact::VolumeSpaceCondition::Low) {
        return VolumePressure::Low;
    }
    return VolumePressure::Normal;
}

Result<SystemCapabilities> build_system_capabilities(const CompositionOptions& o) {
    auto installed = o.plugins->plugins();
    if (!installed.ok()) {
        return installed.status();
    }
    SystemCapabilities caps;
    caps.application_version = RODAQ_APPLICATION_VERSION;
    caps.output_roots.push_back(o.settings.output_root);
    caps.resource_profiles.reserve(o.settings.profiles.size());
    for (const auto& profile : o.settings.profiles) {
        caps.resource_profiles.push_back(
            {profile.name, profile.storage.max_segment_physical_bytes,
             static_cast<std::uint32_t>(o.settings.tasks.config.max_streams_per_pipeline)});
    }
    caps.limits.max_page_size = 256;
    caps.limits.recommended_polling_floor = std::chrono::seconds{1};

    auto find_backend = [&](const std::string& id,
                            const std::string& version) -> BackendCapability* {
        for (auto& entry : caps.backends) {
            if (entry.plugin_id == id && entry.plugin_version == version) {
                return &entry;
            }
        }
        return nullptr;
    };

    for (const auto& plugin : installed.value()) {
        auto* existing = find_backend(plugin.plugin_id, plugin.plugin_version);
        if (existing) {
            existing->installed = true;
            continue;
        }
        BackendCapability entry;
        entry.plugin_id = plugin.plugin_id;
        entry.plugin_version = plugin.plugin_version;
        entry.installed = true;
        entry.allowed = false;
        caps.backends.push_back(std::move(entry));
    }
    for (const auto& allowed : o.settings.allowed_plugins) {
        auto* existing = find_backend(allowed.plugin_id, allowed.plugin_version);
        if (existing) {
            existing->role = to_backend_role(allowed.role);
            existing->allowed = true;
            continue;
        }
        BackendCapability entry;
        entry.plugin_id = allowed.plugin_id;
        entry.plugin_version = allowed.plugin_version;
        entry.role = to_backend_role(allowed.role);
        entry.installed = false;
        entry.allowed = true;
        caps.backends.push_back(std::move(entry));
    }
    for (const auto& qual : o.settings.backend_qualifications) {
        auto* existing = find_backend(qual.plugin_id, qual.plugin_version);
        if (existing) {
            if (!existing->role) {
                existing->role = qual.role;
            }
            existing->support_status = qual.support_status;
            existing->qualification_status = qual.qualification_status;
            continue;
        }
        BackendCapability entry;
        entry.plugin_id = qual.plugin_id;
        entry.plugin_version = qual.plugin_version;
        entry.role = qual.role;
        entry.installed = false;
        entry.allowed = false;
        entry.support_status = qual.support_status;
        entry.qualification_status = qual.qualification_status;
        caps.backends.push_back(std::move(entry));
    }
    std::sort(caps.backends.begin(), caps.backends.end(), [](const auto& a, const auto& b) {
        if (a.plugin_id != b.plugin_id) {
            return a.plugin_id < b.plugin_id;
        }
        return a.plugin_version < b.plugin_version;
    });
    return caps;
}

bool valid(const CompositionOptions& o) {
    if (!o.plugins || !o.volume || !o.capacity || !o.sample || o.task_root.empty() ||
        o.settings.deployment_generation == 0 || o.settings.qualified_runtime_profile_id.empty() ||
        o.settings.qualified_runtime_profile_id.size() > 256 || o.settings.output_root.empty() ||
        o.settings.profiles.empty() || o.settings.profiles.size() > 64 ||
        o.settings.storage.empty() || o.settings.storage.size() > 64 ||
        o.settings.tasks.config.max_pipelines == 0 || o.settings.tasks.config.max_pipelines > 64 ||
        o.settings.max_tasks == 0 || o.settings.max_tasks > 16 ||
        o.settings.max_artifacts_per_volume == 0 || o.settings.max_artifacts_per_volume > 65536 ||
        o.settings.max_tasks * o.settings.tasks.config.max_pipelines > 256 ||
        o.settings.native_timeout <= std::chrono::nanoseconds::zero() ||
        o.settings.system_snapshot_max_age <= std::chrono::nanoseconds::zero() ||
        o.settings.system_snapshot_max_age > std::chrono::hours{24}) {
        return false;
    }
    for (auto i = o.settings.backend_qualifications.begin();
         i != o.settings.backend_qualifications.end(); ++i) {
        if (i->plugin_id.empty() || i->plugin_version.empty() ||
            std::any_of(o.settings.backend_qualifications.begin(), i, [&](const auto& q) {
                return q.plugin_id == i->plugin_id && q.plugin_version == i->plugin_version;
            })) {
            return false;
        }
    }
    for (auto i = o.settings.profiles.begin(); i != o.settings.profiles.end(); ++i) {
        if (i->name.empty() || i->name.size() > 128 || i->segment_reservation_bytes == 0 ||
            i->instance.callback_concurrency == 0 || i->instance.callback_concurrency > 4094 ||
            !artifact::validate_manifest_limits(i->manifest).ok() ||
            std::any_of(
                o.settings.profiles.begin(), i, [&](const auto& p) { return p.name == i->name; })) {
            return false;
        }
    }
    for (auto i = o.settings.storage.begin(); i != o.settings.storage.end(); ++i) {
        if (i->plugin_id.empty() || i->plugin_version.empty() || i->format.empty() ||
            i->format.size() > 128 ||
            std::any_of(
                o.settings.storage.begin(), i,
                [&](const auto& s) {
                    return s.plugin_id == i->plugin_id && s.plugin_version == i->plugin_version;
                }) ||
            !std::any_of(
                o.settings.allowed_plugins.begin(), o.settings.allowed_plugins.end(),
                [&](const auto& p) {
                    return p.role == task::AdmissionRole::Storage && p.plugin_id == i->plugin_id &&
                           p.plugin_version == i->plugin_version;
                })) {
            return false;
        }
    }
    return true;
}

struct CommitReply {
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<Result<void>> result;

    void complete(Result<void> value) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        if (result) {
            return;
        }
        result.emplace(std::move(value));
        ready.notify_all();
    }

    Result<void> wait(std::chrono::nanoseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!ready.wait_for(lock, timeout, [&] { return result.has_value(); })) {
            return Status(StatusCode::DeadlineExceeded, {});
        }
        return *result;
    }
};

struct CommitJob final : Work {
    std::shared_ptr<artifact::ArtifactCatalog> catalog;
    std::optional<artifact::SealedSegment> sealed;
    std::function<void()> on_settled;
    std::shared_ptr<CommitReply> reply;

    void run() noexcept override {
        try {
            if (!sealed) {
                reply->complete(Status(StatusCode::InvalidState, {}));
                return;
            }
            bool settled = false;
            auto result = catalog->commit(std::move(*sealed), &settled);
            sealed.reset();
            if (settled && on_settled) {
                on_settled();
            }
            reply->complete(std::move(result));
        } catch (const std::bad_alloc&) {
            reply->complete(Status(StatusCode::ResourceExhausted, {}));
        } catch (...) {
            reply->complete(Status(StatusCode::Internal, {}));
        }
    }
};

}  // namespace

Result<void> submit_commit(
    const std::shared_ptr<artifact::ArtifactCatalog>& catalog,
    const std::shared_ptr<ExecutionLane>& commit_lane, std::chrono::nanoseconds timeout,
    std::optional<artifact::SealedSegment>& sealed, std::function<void()> on_settled) {
    if (!sealed) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto job = std::make_shared<CommitJob>();
        job->catalog = catalog;
        job->on_settled = std::move(on_settled);
        job->reply = std::make_shared<CommitReply>();
        auto reply = job->reply;
        // All fallible allocations precede the move. Retain the job
        // until queue admission so rejection can restore its owner.
        job->sealed.emplace(std::move(*sealed));
        sealed.reset();
        auto admitted = commit_lane->submit(job);
        if (!admitted.ok()) {
            sealed.emplace(std::move(*job->sealed));
            return admitted.status();
        }
        return reply->wait(timeout);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::unique_ptr<TaskService>> Composition::open_task_service(task::ServiceOptions options) {
    return TaskService::open(std::move(options));
}

struct Composition::State {
    // Declared first: releases the namespace lease after composed authorities.
    std::shared_ptr<DeploymentLease> deployment_lease;
    std::shared_ptr<RunResources> resources;
    std::shared_ptr<artifact::ArtifactCatalog> catalog;
    std::unique_ptr<ArtifactService> artifacts;
    std::unique_ptr<TaskService> tasks;
    std::unique_ptr<SystemService> system;
    std::vector<std::shared_ptr<ExecutionLane>> lanes;
    std::mutex lifecycle;
    std::mutex observation;
    bool started{false};
    std::atomic<bool> core_ready{false};
    std::atomic<bool> stopping{false};
    bool stopped{false};
    std::atomic<std::uint64_t> last_total_bytes{0};

    ~State() noexcept {
        if (started && !stopped) {
            std::terminate();
        }
        if (!started) {
            for (const auto& lane : lanes) {
                (void)lane->shutdown(std::chrono::steady_clock::now());
            }
        }
    }
};

void Composition::publish_system_observation(
    const artifact::VolumeSpaceSnapshot& space, std::uint64_t total_bytes,
    bool force_protected) noexcept {
    if (!state_->system) {
        return;
    }
    try {
        const auto& o = state_->resources->options;
        const auto cap = o.capacity->snapshot();
        const auto cat = state_->catalog->snapshot();
        const bool ready = state_->core_ready.load(std::memory_order_acquire) &&
                           !state_->stopping.load(std::memory_order_acquire);
        system::SystemObservation obs;
        obs.core_ready = ready;
        VolumeStatus volume;
        volume.name = o.settings.output_root;
        volume.total_bytes = total_bytes;
        volume.free_bytes = force_protected ? 0 : space.effective_free_bytes;
        volume.pressure = to_volume_pressure(space, cap, cat, force_protected);
        obs.volumes.push_back(std::move(volume));
        obs.backlog.commit_items = std::max<std::uint64_t>(
            cap.commit_slots, static_cast<std::uint64_t>(cat.commit_inflight));
        obs.backlog.commit_bytes = cap.commit_bytes;
        if (!state_->system->refresh(obs).ok()) {
            state_->system->mark_observation_degraded();
        }
    } catch (...) {
        state_->system->mark_observation_degraded();
    }
}

Composition::Composition(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

Composition::~Composition() noexcept = default;

void Composition::retain_lease(std::shared_ptr<DeploymentLease> lease) noexcept {
    state_->deployment_lease = std::move(lease);
}

Result<std::unique_ptr<Composition>> Composition::create(CompositionOptions options) {
    try {
        if (!valid(options)) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto verified = options.volume->verify();
        if (!verified.ok()) {
            return verified.status();
        }
        auto state = std::make_unique<State>();
        state->resources = std::make_shared<RunResources>();
        state->resources->options = std::move(options);
        const auto& o = state->resources->options;

        // Six bounded business domains + per-Pipeline Writers:
        // [0] create (1), [1] control (max_tasks), [2] prepare/rotate (max_tasks),
        // [3] task cleanup (max_tasks), [4] parallel per-pipeline cleanup,
        // [5] single-Volume commit (1).
        const auto writer_count = o.settings.max_tasks * o.settings.tasks.config.max_pipelines;
        constexpr std::size_t kCoreDomains = 6;
        state->lanes.reserve(kCoreDomains + writer_count);
        state->resources->writers.reserve(writer_count);
        state->resources->writer_tasks.resize(o.settings.max_tasks);
        std::size_t writer_capacity = 2;
        for (const auto& profile : o.settings.profiles) {
            writer_capacity = std::max(
                writer_capacity,
                static_cast<std::size_t>(profile.instance.callback_concurrency) + 2);
        }
        std::size_t commit_queue = 8;
        if (auto vol_policy = o.capacity->policy(); vol_policy.ok()) {
            const auto slots = static_cast<std::size_t>(vol_policy.value().max_commit_slots);
            commit_queue = std::clamp<std::size_t>(slots * 2, 8, 4096);
        }
        const std::size_t pipeline_cleanup_workers = std::clamp<std::size_t>(writer_count, 2, 16);
        const std::size_t pipeline_cleanup_queue =
            std::clamp<std::size_t>(writer_count * 2, 8, 4096);
        for (std::size_t i = 0; i < kCoreDomains + writer_count; ++i) {
            std::size_t workers = 1;
            std::size_t capacity = writer_capacity;
            if (i == 0) {
                workers = 1;
                capacity = std::max<std::size_t>(8, o.settings.max_tasks);
            } else if (i == 1) {
                workers = o.settings.max_tasks;
                capacity = std::max<std::size_t>(8, o.settings.max_tasks * 2);
            } else if (i == 2 || i == 3) {
                workers = o.settings.max_tasks;
                capacity = std::max<std::size_t>(8, o.settings.max_tasks);
            } else if (i == 4) {
                workers = pipeline_cleanup_workers;
                capacity = pipeline_cleanup_queue;
            } else if (i == 5) {
                workers = 1;
                capacity = commit_queue;
            }
            auto lane = ExecutionLane::create(workers, capacity);
            if (!lane.ok()) {
                return lane.status();
            }
            state->lanes.push_back(lane.value());
            if (i >= kCoreDomains) {
                state->resources->writers.push_back(std::move(lane.value()));
            }
        }

        // Synchronous bounded startup scan before Server becomes Ready.
        artifact::CatalogOptions catalog;
        catalog.volume = o.volume;
        catalog.capacity = o.capacity;
        catalog.max_artifacts = o.settings.max_artifacts_per_volume;
        catalog.durability = o.settings.durability;
        catalog.manifest = catalog_manifest_limits(o.settings);
        auto index = artifact::ArtifactCatalog::create(std::move(catalog));
        if (!index.ok()) {
            return index.status();
        }
        state->catalog = std::move(index.value());
        auto scanned = state->catalog->startup_scan();
        if (!scanned.ok()) {
            return scanned.status();
        }
        auto artifacts = ArtifactService::create(state->catalog);
        if (!artifacts.ok()) {
            return artifacts.status();
        }
        state->artifacts = std::move(artifacts.value());

        task::ServiceOptions service;
        service.root = o.task_root;
        service.plugin_manager = o.plugins;
        service.create_lane = state->lanes[0];
        service.control_lane = state->lanes[1];
        service.max_tasks = o.settings.max_tasks;
        service.store = o.settings.tasks;
        service.file_faults = o.task_file_faults;
        service.allowed_plugins = o.settings.allowed_plugins;
        service.output_roots = {o.settings.output_root};
        for (const auto& profile : o.settings.profiles) {
            service.resource_profiles.push_back(
                {profile.name, profile.storage.max_segment_physical_bytes});
        }
        auto execution = std::make_shared<task::RunExecutionOptions>();
        execution->sample = o.sample;
        execution->preparation_lane = state->lanes[2];
        execution->cleanup_lane = state->lanes[3];
        execution->pipeline_cleanup_lane = state->lanes[4];
        execution->native_timeout = o.settings.native_timeout;
        execution->resolve = [resources = std::shared_ptr<const RunResources>(state->resources)](
                                 TaskId task_id, std::string_view config_version,
                                 ControlGeneration gen, const TaskConfig& config) {
            return resolve_run(*resources, task_id, config_version, gen, config);
        };
        execution->release_task_resources =
            [resources = std::shared_ptr<const RunResources>(state->resources)](
                TaskId task_id) noexcept { release_writer_slot(*resources, task_id); };
        execution->commit_with_ack = [catalog = state->catalog, commit_lane = state->lanes[5],
                                      timeout = o.settings.native_timeout](
                                         std::optional<artifact::SealedSegment>& sealed,
                                         std::function<void()> on_settled) -> Result<void> {
            return submit_commit(catalog, commit_lane, timeout, sealed, std::move(on_settled));
        };
        execution->abort_sealed = [catalog = state->catalog](artifact::SealedSegment& sealed) {
            return catalog->abort(sealed);
        };
        execution->allocate_segment_id = [volume = o.volume]() {
            return volume->allocate_segment_id();
        };
        execution->artifacts.volume_id = o.volume->identity();
        execution->artifacts.reset_targets = [catalog = state->catalog](TaskId task_id) {
            return catalog->reset_targets(task_id);
        };
        execution->artifacts.delete_for_reset =
            [catalog = state->catalog](
                ArtifactId artifact_id, std::chrono::steady_clock::time_point deadline) {
                return catalog->delete_artifact(artifact_id, deadline);
            };
        execution->artifacts.retire_eligible = [catalog = state->catalog](TaskId task_id) {
            return catalog->retire_eligible(task_id);
        };
        service.execution = std::move(execution);
        auto tasks = open_task_service(std::move(service));
        if (!tasks.ok()) {
            return tasks.status();
        }
        state->tasks = std::move(tasks.value());

        auto capabilities = build_system_capabilities(o);
        if (!capabilities.ok()) {
            return capabilities.status();
        }
        system::SystemServiceOptions system_options;
        system_options.capabilities = std::move(capabilities.value());
        system_options.snapshot_max_age = o.settings.system_snapshot_max_age;
        auto system = SystemService::create(std::move(system_options));
        if (!system.ok()) {
            return system.status();
        }
        state->system = std::move(system.value());

        auto composition = std::unique_ptr<Composition>(new Composition(std::move(state)));
        auto observed = composition->observe();
        if (!observed.ok()) {
            return observed.status();
        }
        auto boot_sample = composition->state_->resources->options.sample();
        if (!boot_sample.ok()) {
            return boot_sample.status();
        }
        const auto boot_space = composition->state_->resources->options.capacity->space_snapshot(
            boot_sample.value().monotonic_ns);
        composition->state_->core_ready.store(true, std::memory_order_release);
        composition->publish_system_observation(
            boot_space, composition->state_->last_total_bytes.load(std::memory_order_acquire),
            false);
        composition->state_->started = true;
        return composition;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

TaskService& Composition::tasks() noexcept { return *state_->tasks; }

ArtifactService& Composition::artifacts() noexcept { return *state_->artifacts; }

SystemService& Composition::system() noexcept { return *state_->system; }

Result<void> Composition::observe() {
    std::lock_guard<std::mutex> serial(state_->observation);
    if (state_->stopping) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto& o = state_->resources->options;
    if (state_->deployment_lease) {
        auto verified = state_->deployment_lease->verify();
        if (!verified.ok()) {
            (void)state_->tasks->refresh_execution_facts(true);
            publish_system_observation(
                {}, state_->last_total_bytes.load(std::memory_order_acquire), true);
            return verified.status();
        }
    }
    auto sample = o.sample();
    if (state_->stopping.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, {});
    }
    if (!sample.ok()) {
        o.capacity->invalidate_space_sample();
        (void)state_->tasks->refresh_execution_facts(true);
        (void)state_->tasks->reconcile_volume(artifact::VolumeSpaceCondition::Unknown, 0);
        publish_system_observation(
            {}, state_->last_total_bytes.load(std::memory_order_acquire), true);
        return sample.status();
    }
    auto probe = o.capacity->begin_space_sample(sample.value().monotonic_ns);
    if (!probe.ok()) {
        return probe.status();
    }
    auto stats = o.volume->space_stats();
    auto completed = o.sample();
    if (state_->stopping.load(std::memory_order_acquire)) {
        o.capacity->invalidate_space_sample();
        return Status(StatusCode::InvalidState, {});
    }
    if (!stats.ok() || !completed.ok()) {
        o.capacity->invalidate_space_sample();
        (void)state_->tasks->refresh_execution_facts(true);
        (void)state_->tasks->reconcile_volume(
            artifact::VolumeSpaceCondition::Unknown, sample.value().monotonic_ns);
        publish_system_observation(
            {}, state_->last_total_bytes.load(std::memory_order_acquire), true);
        return !stats.ok() ? stats.status() : completed.status();
    }
    state_->last_total_bytes.store(stats.value().total_bytes, std::memory_order_release);
    auto updated = o.capacity->finish_space_sample(
        probe.value(), stats.value().available_bytes, completed.value().monotonic_ns);
    if (!updated.ok()) {
        o.capacity->invalidate_space_sample();
        (void)state_->tasks->refresh_execution_facts(true, completed.value().monotonic_ns);
        (void)state_->tasks->reconcile_volume(
            artifact::VolumeSpaceCondition::Unknown, completed.value().monotonic_ns);
        publish_system_observation({}, stats.value().total_bytes, true);
        return updated.status();
    }
    const auto space = o.capacity->space_snapshot(completed.value().monotonic_ns);
    const bool protect = space.condition != artifact::VolumeSpaceCondition::Normal &&
                         space.condition != artifact::VolumeSpaceCondition::Low;
    auto refreshed =
        state_->tasks->refresh_execution_facts(protect, completed.value().monotonic_ns);
    if (!refreshed.ok()) {
        publish_system_observation(space, stats.value().total_bytes, true);
        return refreshed.status();
    }
    auto cleanup = state_->tasks->drain_shutdown(true);
    if (!cleanup.ok()) {
        publish_system_observation(space, stats.value().total_bytes, false);
        return cleanup.status();
    }
    auto recovery =
        state_->tasks->reconcile_volume(space.condition, completed.value().monotonic_ns);
    if (!recovery.ok()) {
        publish_system_observation(space, stats.value().total_bytes, false);
        return recovery.status();
    }
    auto rotated = state_->tasks->schedule_rotation(completed.value().monotonic_ns);
    if (!rotated.ok()) {
        publish_system_observation(space, stats.value().total_bytes, false);
        return rotated.status();
    }
    auto retained = state_->tasks->apply_retention(*state_->catalog);
    publish_system_observation(space, stats.value().total_bytes, false);
    return retained;
}

Result<void> Composition::shutdown(std::chrono::steady_clock::time_point deadline) {
    std::lock_guard<std::mutex> serial(state_->lifecycle);
    if (state_->stopped) {
        return {};
    }
    state_->core_ready.store(false, std::memory_order_release);
    state_->stopping = true;
    publish_system_observation({}, state_->last_total_bytes.load(std::memory_order_acquire), true);
    auto closed = state_->tasks->close_admission();
    if (!closed.ok()) {
        return closed.status();
    }
    state_->catalog->cancel_readers();
    // Initiate drain/seal/commit BEFORE waiting on global Create/Control idle (IC-T03).
    auto draining = state_->tasks->drain_shutdown();
    const Status initial_drain = draining.ok() ? Status{} : draining.status();
    (void)state_->lanes[3]->wait_idle(deadline);
    (void)state_->lanes[4]->wait_idle(deadline);
    (void)state_->lanes[5]->wait_idle(deadline);

    auto created = state_->lanes[0]->wait_idle(deadline);
    if (!created.ok()) {
        return created.status();
    }
    auto controls = state_->lanes[1]->wait_idle(deadline);
    if (!controls.ok()) {
        return controls.status();
    }
    auto prepared = state_->lanes[2]->shutdown(deadline);
    if (!prepared.ok()) {
        return prepared.status();
    }
    draining = state_->tasks->drain_shutdown();
    if (!draining.ok()) {
        return draining.status();
    }
    if (!initial_drain.ok()) {
        return initial_drain;
    }
    auto cleaned = state_->lanes[3]->wait_idle(deadline);
    if (!cleaned.ok()) {
        return cleaned.status();
    }
    auto pipeline_cleaned = state_->lanes[4]->wait_idle(deadline);
    if (!pipeline_cleaned.ok()) {
        return pipeline_cleaned.status();
    }
    auto committed = state_->lanes[5]->wait_idle(deadline);
    if (!committed.ok()) {
        return committed.status();
    }
    created = state_->lanes[0]->wait_idle(deadline);
    if (!created.ok()) {
        return created.status();
    }
    controls = state_->lanes[1]->wait_idle(deadline);
    if (!controls.ok()) {
        return controls.status();
    }
    if (!state_->tasks->shutdown_settled()) {
        return Status(StatusCode::Unavailable, {});
    }
    for (const std::size_t idx :
         {std::size_t{2}, std::size_t{3}, std::size_t{4}, std::size_t{5}, std::size_t{0},
          std::size_t{1}}) {
        auto quiet = state_->lanes[idx]->shutdown(deadline);
        if (!quiet.ok()) {
            return quiet.status();
        }
    }
    for (const auto& writer : state_->resources->writers) {
        auto quiet = writer->shutdown(deadline);
        if (!quiet.ok()) {
            return quiet.status();
        }
    }
    auto remaining = deadline - std::chrono::steady_clock::now();
    auto native_deadline = plugin::Deadline::after(std::max(
        std::chrono::nanoseconds::zero(),
        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining)));
    if (!native_deadline.ok()) {
        return native_deadline.status();
    }
    auto plugins = state_->resources->options.plugins->shutdown(native_deadline.value());
    if (!plugins.ok()) {
        return plugins.status();
    }
    auto refreshed = state_->tasks->refresh_execution_facts();
    if (!refreshed.ok()) {
        return refreshed.status();
    }
    state_->stopped = true;
    return {};
}

}  // namespace rodaq::daq
