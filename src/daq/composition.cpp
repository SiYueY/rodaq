#include "composition.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "run_configuration.hpp"
#include "../artifact/artifact_catalog.hpp"

namespace rodaq::daq {
namespace {

bool valid(const CompositionOptions& o) {
    const auto boot = time::parse_boot_identity(o.boot.kernel_uuid);
    if (!o.plugins || !o.clock || !o.volume || !o.capacity || !o.ledger || !o.segment_ids ||
        !o.sample || !boot.ok() || boot.value().id != o.boot.id ||
        o.clock->anchor().boot_id != o.boot.id || o.task_root.empty() ||
        o.deployment_generation == 0 || o.qualified_runtime_profile_id.empty() ||
        o.qualified_runtime_profile_id.size() > 256 || o.output_root.empty() ||
        o.profiles.empty() || o.profiles.size() > 64 || o.storage.empty() ||
        o.storage.size() > 64 || o.tasks.config.max_pipelines == 0 ||
        o.tasks.config.max_pipelines > 64 || o.native_timeout <= std::chrono::nanoseconds::zero()) {
        return false;
    }
    for (auto i = o.profiles.begin(); i != o.profiles.end(); ++i) {
        if (i->name.empty() || i->name.size() > 128 || i->sequence_reservation == 0 ||
            i->descriptor_reservation == 0 || i->instance.callback_concurrency == 0 ||
            i->instance.callback_concurrency > 4094 ||
            !artifact::validate_manifest_limits(i->manifest).ok() ||
            std::any_of(o.profiles.begin(), i, [&](const auto& p) { return p.name == i->name; })) {
            return false;
        }
    }
    for (auto i = o.storage.begin(); i != o.storage.end(); ++i) {
        if (i->plugin_id.empty() || i->plugin_version.empty() || i->format.empty() ||
            i->format.size() > 128 ||
            std::any_of(
                o.storage.begin(), i,
                [&](const auto& s) {
                    return s.plugin_id == i->plugin_id && s.plugin_version == i->plugin_version;
                }) ||
            !std::any_of(o.allowed_plugins.begin(), o.allowed_plugins.end(), [&](const auto& p) {
                return p.role == task::AdmissionRole::Storage && p.plugin_id == i->plugin_id &&
                       p.plugin_version == i->plugin_version;
            })) {
            return false;
        }
    }
    return true;
}

}  // namespace

struct Composition::State {
    std::shared_ptr<RunResources> resources;
    std::shared_ptr<artifact::ArtifactCatalog> catalog;
    std::unique_ptr<ArtifactService> artifacts;
    std::unique_ptr<TaskService> tasks;
    std::vector<std::shared_ptr<ExecutionLane>> lanes;
    // Only serializes explicit lifecycle/maintenance calls, never Task APIs.
    std::mutex lifecycle;
    std::mutex observation;
    bool started{false};
    std::atomic<bool> stopping{false};
    bool stopped{false};

    ~State() noexcept {
        if (started && !stopped) {
            std::terminate();
        }
        if (!started) {
            // Construction submitted no work. Roll back idle prepared domains.
            for (const auto& lane : lanes) {
                (void)lane->shutdown(std::chrono::steady_clock::now());
            }
        }
    }
};

Composition::Composition(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

Composition::~Composition() noexcept = default;

Result<std::unique_ptr<Composition>> Composition::create(CompositionOptions options) {
    try {
        if (!valid(options)) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto verified = options.volume->verify();
        if (!verified.ok()) {
            return verified.status();
        }
        auto clock = options.clock->snapshot();
        if (!clock.ok()) {
            return clock.status();
        }
        auto state = std::make_unique<State>();
        state->resources = std::make_shared<RunResources>();
        state->resources->options = std::move(options);
        const auto& o = state->resources->options;
        // Reserve before creating any thread so every domain has an owner even
        // if a later allocation fails. Four distinct business scheduling lanes.
        state->lanes.reserve(4 + o.tasks.config.max_pipelines);
        state->resources->writers.reserve(o.tasks.config.max_pipelines);
        std::size_t writer_capacity = 2;
        for (const auto& profile : o.profiles) {
            writer_capacity = std::max(
                writer_capacity,
                static_cast<std::size_t>(profile.instance.callback_concurrency) + 2);
        }
        for (std::size_t i = 0; i < 4 + o.tasks.config.max_pipelines; ++i) {
            auto lane = ExecutionLane::create(1, i < 4 ? 8 : writer_capacity);
            if (!lane.ok()) {
                return lane.status();
            }
            state->lanes.push_back(lane.value());
            if (i >= 4) {
                state->resources->writers.push_back(std::move(lane.value()));
            }
        }
        artifact::CatalogOptions catalog;
        catalog.volume = o.volume;
        catalog.capacity = o.capacity;
        catalog.ledger = o.ledger;
        catalog.max_artifacts = o.ledger->capacity();
        catalog.manifest = o.profiles.front().manifest;
        for (const auto& p : o.profiles) {
            catalog.manifest.max_bytes = std::max(catalog.manifest.max_bytes, p.manifest.max_bytes);
            catalog.manifest.max_files = std::max(catalog.manifest.max_files, p.manifest.max_files);
            catalog.manifest.max_descriptors =
                std::max(catalog.manifest.max_descriptors, p.manifest.max_descriptors);
            catalog.manifest.max_streams =
                std::max(catalog.manifest.max_streams, p.manifest.max_streams);
            catalog.manifest.max_epochs =
                std::max(catalog.manifest.max_epochs, p.manifest.max_epochs);
            catalog.manifest.max_qos_profiles =
                std::max(catalog.manifest.max_qos_profiles, p.manifest.max_qos_profiles);
        }
        auto index = artifact::ArtifactCatalog::create(std::move(catalog));
        if (!index.ok()) {
            return index.status();
        }
        state->catalog = std::move(index.value());
        auto rebuilt = state->catalog->rebuild();
        if (!rebuilt.ok()) {
            return rebuilt.status();
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
        service.max_tasks = 1;
        service.store = o.tasks;
        service.allowed_plugins = o.allowed_plugins;
        service.output_roots = {o.output_root};
        for (const auto& profile : o.profiles) {
            service.resource_profiles.push_back(profile.name);
        }
        auto execution = std::make_shared<task::RunExecutionOptions>();
        execution->preparation_lane = state->lanes[2];
        execution->cleanup_lane = state->lanes[3];
        execution->native_timeout = o.native_timeout;
        execution->resolve = [resources = std::shared_ptr<const RunResources>(state->resources)](
                                 const auto& control, const auto& config, auto& identities) {
            return resolve_run(*resources, control, config, identities);
        };
        execution->commit = [catalog = state->catalog](const auto& sealed) {
            return catalog->commit(sealed);
        };
        service.execution = std::move(execution);
        auto tasks = TaskService::open(std::move(service));
        if (!tasks.ok()) {
            return tasks.status();
        }
        state->tasks = std::move(tasks.value());
        // Allocate the final facade before enabling its mandatory-shutdown rule.
        auto composition = std::unique_ptr<Composition>(new Composition(std::move(state)));
        auto observed = composition->observe();
        if (!observed.ok()) {
            return observed.status();
        }
        composition->state_->started = true;
        return composition;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

TaskService& Composition::tasks() noexcept { return *state_->tasks; }

ArtifactService& Composition::artifacts() noexcept { return *state_->artifacts; }

Result<void> Composition::observe() {
    std::lock_guard<std::mutex> serial(state_->observation);
    if (state_->stopping) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto& o = state_->resources->options;
    auto sample = o.sample();
    if (state_->stopping.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, {});
    }
    if (!sample.ok()) {
        (void)state_->tasks->refresh_execution_facts(true);
        return sample.status();
    }
    auto observed = o.clock->observe(sample.value());
    if (!observed.ok()) {
        (void)state_->tasks->refresh_execution_facts(true);
        return observed.status();
    }
    auto probe = o.capacity->begin_space_sample(sample.value().monotonic_ns);
    if (!probe.ok()) {
        return probe.status();
    }
    auto free = o.volume->available_bytes();
    auto completed = o.sample();
    if (state_->stopping.load(std::memory_order_acquire)) {
        o.capacity->invalidate_space_sample();
        return Status(StatusCode::InvalidState, {});
    }
    if (!free.ok() || !completed.ok()) {
        o.capacity->invalidate_space_sample();
        (void)state_->tasks->refresh_execution_facts(true);
        return !free.ok() ? free.status() : completed.status();
    }
    auto updated = o.capacity->finish_space_sample(
        probe.value(), free.value(), completed.value().monotonic_ns);
    if (!updated.ok()) {
        return updated.status();
    }
    return state_->tasks->refresh_execution_facts();
}

Result<void> Composition::shutdown(std::chrono::steady_clock::time_point deadline) {
    std::lock_guard<std::mutex> serial(state_->lifecycle);
    if (state_->stopped) {
        return {};
    }
    state_->stopping = true;
    auto closed = state_->tasks->close_admission();
    if (!closed.ok()) {
        return closed.status();
    }
    state_->catalog->cancel_readers();
    // Keep control and cleanup admission until actual completions have been
    // published. In particular, an observer timeout must leave cleanup retryable.
    auto created = state_->lanes[0]->shutdown(deadline);
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
    auto draining = state_->tasks->drain_shutdown();
    if (!draining.ok()) {
        return draining.status();
    }
    auto cleaned = state_->lanes[3]->wait_idle(deadline);
    if (!cleaned.ok()) {
        return cleaned.status();
    }
    controls = state_->lanes[1]->wait_idle(deadline);
    if (!controls.ok()) {
        return controls.status();
    }
    if (!state_->tasks->shutdown_settled()) {
        // A failed SDK create has no public per-create completion handle.
        // Only actual Manager shutdown can resolve its private Native owner.
        // Do this while cleanup/Writer domains are still available, then let
        // the same Task owners reconcile that evidence rather than asserting
        // that a failed create necessarily left no instance behind.
        auto native_deadline = plugin::Deadline::after(std::max(
            std::chrono::nanoseconds::zero(), std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  deadline - std::chrono::steady_clock::now())));
        if (!native_deadline.ok()) {
            return native_deadline.status();
        }
        auto native = state_->resources->options.plugins->shutdown(native_deadline.value());
        if (!native.ok()) {
            return native.status();
        }
        draining = state_->tasks->drain_shutdown();
        if (!draining.ok()) {
            return draining.status();
        }
        cleaned = state_->lanes[3]->wait_idle(deadline);
        if (!cleaned.ok()) {
            return cleaned.status();
        }
        if (!state_->tasks->shutdown_settled()) {
            return Status(StatusCode::Unavailable, {});
        }
    }
    for (std::size_t i = 1; i < 4; ++i) {
        auto quiet = state_->lanes[i]->shutdown(deadline);
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
