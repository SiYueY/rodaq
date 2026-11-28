#include "task_service_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../uuid.hpp"
#include "config.hpp"
#include "identity.hpp"

namespace rodaq {
namespace {

bool valid_options(const task::ServiceOptions& options) {
    return !options.root.empty() && options.plugin_manager && options.create_lane &&
           options.control_lane && options.max_tasks > 0 && options.max_tasks <= 64 &&
           options.max_requests > 0 && options.max_requests <= 1024 &&
           options.request_wait > std::chrono::nanoseconds::zero() &&
           options.runtime_snapshot_max_age > std::chrono::nanoseconds::zero();
}

TaskSummary build_summary(const task::PublishedTask& pub, std::chrono::nanoseconds max_age) {
    TaskSummary summary;
    summary.task = pub.snapshot;
    const auto now = std::chrono::steady_clock::now();
    if (summary.task.freshness != SnapshotFreshness::NotObserved) {
        auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(now - pub.observed_at);
        if (age < std::chrono::nanoseconds::zero()) {
            age = std::chrono::nanoseconds::zero();
        }
        summary.task.age = age;
        if (summary.task.freshness == SnapshotFreshness::Fresh && age > max_age) {
            summary.task.freshness = SnapshotFreshness::Stale;
        }
    }
    summary.freshness = summary.task.freshness;
    summary.data_impact = summary.task.data_impact;
    summary.accepted_records_per_second = pub.runtime.accepted_records_per_second;
    for (const auto& pipe : pub.runtime.pipelines) {
        for (const auto& stream : pipe.streams) {
            ++summary.total_streams;
            if (stream.facts && stream.facts->receiving.value_or(false)) {
                ++summary.receiving_streams;
            }
        }
    }
    return summary;
}

}  // namespace

Result<task::RequestSlot> TaskService::State::enter_request() {
    if (!accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::Unavailable, "TaskService is shutting down");
    }
    auto current = requests.load(std::memory_order_relaxed);
    while (current < options->max_requests) {
        if (requests.compare_exchange_weak(
                current, current + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            if (!accepting.load(std::memory_order_acquire)) {
                requests.fetch_sub(1, std::memory_order_release);
                return Status(StatusCode::Unavailable, "TaskService is shutting down");
            }
            return task::RequestSlot(requests);
        }
    }
    return Status(StatusCode::ResourceExhausted, "Too many concurrent TaskService requests");
}

Result<void> TaskService::State::validate_deployment(const TaskConfig& config) const {
    auto valid = task::validate_task_config(config, options->store.config, false);
    if (!valid.ok()) {
        return valid.status();
    }
    const auto allowed = [&](const PluginConfig& plugin, task::AdmissionRole role) {
        const bool admitted = std::any_of(
            options->allowed_plugins.begin(), options->allowed_plugins.end(),
            [&](const auto& item) {
                return item.role == role && item.plugin_id == plugin.plugin_id &&
                       item.plugin_version == plugin.plugin_version;
            });
        const bool present = std::any_of(installed.begin(), installed.end(), [&](const auto& item) {
            return item.plugin_id == plugin.plugin_id &&
                   item.plugin_version == plugin.plugin_version;
        });
        return admitted && present;
    };
    for (const auto& pipe : config.pipelines) {
        if (!allowed(pipe.acquisition, task::AdmissionRole::Acquisition) ||
            !allowed(pipe.storage, task::AdmissionRole::Storage)) {
            return Status(StatusCode::PermissionDenied, "Plugin not allowed or not installed");
        }
        const auto profile = std::find_if(
            options->resource_profiles.begin(), options->resource_profiles.end(),
            [&](const auto& item) { return item.name == pipe.resource_profile; });
        if (profile == options->resource_profiles.end()) {
            return Status(StatusCode::PermissionDenied, "Unknown resource_profile");
        }
        if (config.segment_policy.max_physical_bytes && profile->max_segment_physical_bytes != 0 &&
            *config.segment_policy.max_physical_bytes > profile->max_segment_physical_bytes) {
            return Status(
                StatusCode::InvalidArgument,
                "segment_policy.max_physical_bytes exceeds resource profile limit");
        }
        if (std::find(
                options->output_roots.begin(), options->output_roots.end(), pipe.output_root) ==
            options->output_roots.end()) {
            return Status(StatusCode::PermissionDenied, "Unknown output_root");
        }
    }
    return {};
}

std::shared_ptr<task::TaskEntry> TaskService::State::find(TaskId id) {
    std::lock_guard<std::mutex> lock(catalog_mutex);
    for (const auto& entry : entries) {
        if (entry->id == id) {
            return entry;
        }
    }
    return nullptr;
}

void TaskService::State::publish_locked(task::TaskEntry& entry) noexcept {
    try {
        const auto previous = std::atomic_load(&entry.published);
        auto pub = task::make_task_publication(
            entry, entry.config, entry.snapshot_sequence++, previous.get());
        if (pub.ok()) {
            std::atomic_store(&entry.published, std::move(pub.value()));
        }
    } catch (...) {
    }
}

TaskService::TaskService(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

TaskService::~TaskService() noexcept = default;

Result<std::unique_ptr<TaskService>> TaskService::open(task::ServiceOptions options) {
    if (!valid_options(options)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto installed = options.plugin_manager->plugins();
        if (!installed.ok()) {
            return installed.status();
        }
        auto store =
            task::TaskConfigStore::open(options.root / "tasks", options.store, options.file_faults);
        if (!store.ok()) {
            return store.status();
        }
        auto loaded = store.value().load_all();
        if (!loaded.ok()) {
            return loaded.status();
        }
        if (loaded.value().size() > options.max_tasks) {
            return Status(StatusCode::ResourceExhausted, "Stored tasks exceed max_tasks");
        }
        auto state = std::make_shared<State>();
        state->options = std::make_shared<const task::ServiceOptions>(std::move(options));
        state->installed = std::move(installed.value());
        state->store.emplace(std::move(store.value()));
        state->create_registry = task::CreateRegistry(state->options->store.history);
        state->entries.reserve(state->options->max_tasks);

        for (auto& item : loaded.value()) {
            auto entry = std::make_shared<task::TaskEntry>(state->options->store.history);
            entry->id = item.metadata.task_id;
            entry->metadata = std::move(item.metadata);
            entry->config = std::make_shared<const TaskConfig>(std::move(item.active_config));
            entry->desired_state = DesiredState::Stopped;
            entry->control_generation = 1;
            state->publish_locked(*entry);
            state->entries.push_back(std::move(entry));
        }
        return std::unique_ptr<TaskService>(new TaskService(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskId> TaskService::create_task(const TaskConfig& config, const RequestIdentity& request) {
    auto slot = state_->enter_request();
    if (!slot.ok()) {
        return slot.status();
    }
    auto valid_req = task::validate_request_identity(request);
    if (!valid_req.ok()) {
        return valid_req.status();
    }
    auto valid_cfg = state_->validate_deployment(config);
    if (!valid_cfg.ok()) {
        return valid_cfg.status();
    }

    const auto now = task::control_wall_now();
    {
        std::lock_guard<std::mutex> lock(state_->create_mutex);
        auto existing = state_->create_registry.find(request.request_key, config, now);
        if (existing.ok()) {
            return existing.value();
        }
        if (existing.status().code() != StatusCode::NotFound) {
            return existing.status();
        }
    }

    struct CreateJob final : Work {
        std::shared_ptr<State> owner;
        TaskConfig config;
        RequestIdentity request;
        std::shared_ptr<task::Reply<TaskId>> reply;

        void run() noexcept override {
            try {
                const auto run_now = task::control_wall_now();
                {
                    std::lock_guard<std::mutex> lock(owner->create_mutex);
                    auto existing =
                        owner->create_registry.find(request.request_key, config, run_now);
                    if (existing.ok()) {
                        reply->complete(existing.value());
                        return;
                    }
                    if (existing.status().code() != StatusCode::NotFound) {
                        reply->complete(existing.status());
                        return;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(owner->catalog_mutex);
                    if (owner->entries.size() >= owner->options->max_tasks) {
                        reply->complete(
                            Status(StatusCode::ResourceExhausted, "Task limit reached"));
                        return;
                    }
                }
                auto id = task::new_task_id();
                if (!id.ok()) {
                    reply->complete(id.status());
                    return;
                }
                Result<task::LoadedTask> persisted(Status(StatusCode::Internal, {}));
                {
                    std::lock_guard<std::mutex> slock(owner->store_mutex);
                    persisted = owner->store->create_task(id.value(), config);
                }
                if (!persisted.ok()) {
                    reply->complete(persisted.status());
                    return;
                }
                auto entry = std::make_shared<task::TaskEntry>(owner->options->store.history);
                entry->id = persisted.value().metadata.task_id;
                entry->metadata = std::move(persisted.value().metadata);
                entry->config =
                    std::make_shared<const TaskConfig>(std::move(persisted.value().active_config));
                entry->desired_state = DesiredState::Stopped;
                entry->control_generation = 1;
                owner->publish_locked(*entry);
                {
                    std::lock_guard<std::mutex> lock(owner->catalog_mutex);
                    if (owner->entries.size() >= owner->options->max_tasks) {
                        lock.~lock_guard();
                        std::lock_guard<std::mutex> slock(owner->store_mutex);
                        (void)owner->store->remove_task(entry->id);
                        reply->complete(
                            Status(StatusCode::ResourceExhausted, "Task limit reached"));
                        return;
                    }
                    owner->entries.push_back(entry);
                }
                {
                    std::lock_guard<std::mutex> lock(owner->create_mutex);
                    (void)owner->create_registry.record(
                        request.request_key, config, entry->id, run_now);
                }
                reply->complete(entry->id);
            } catch (const std::bad_alloc&) {
                reply->complete(Status(StatusCode::ResourceExhausted, {}));
            } catch (...) {
                reply->complete(Status(StatusCode::Internal, {}));
            }
        }
    };

    try {
        auto job = std::make_shared<CreateJob>();
        job->owner = state_;
        job->config = config;
        job->request = request;
        job->reply = std::make_shared<task::Reply<TaskId>>();
        auto reply = job->reply;
        auto submitted = state_->options->create_lane->submit(std::move(job));
        if (!submitted.ok()) {
            return submitted.status();
        }
        return reply->wait(state_->options->request_wait);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationId> TaskService::configure(
    TaskId task_id, const TaskConfig& config, const RequestIdentity& request,
    const ControlPrecondition& precondition) {
    auto slot = state_->enter_request();
    if (!slot.ok()) {
        return slot.status();
    }
    if (!uuid::nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto valid_req = task::validate_request_identity(request);
    if (!valid_req.ok()) {
        return valid_req.status();
    }
    auto valid_cfg = state_->validate_deployment(config);
    if (!valid_cfg.ok()) {
        return valid_cfg.status();
    }
    auto entry = state_->find(task_id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }

    const task::RequestSignature sig{OperationKind::Configure, precondition, config};
    const auto now = task::control_wall_now();

    task::TaskMetadata base_meta;
    OperationSnapshot op;
    {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        if (entry->retired) {
            return Status(StatusCode::NotFound, {});
        }
        auto dup = entry->operations.find_request(request.request_key, sig, now);
        if (dup.ok()) {
            return dup.value();
        }
        if (dup.status().code() != StatusCode::NotFound) {
            return dup.status();
        }
        if (precondition.has_expected_control_generation &&
            precondition.expected_control_generation != entry->control_generation) {
            return Status(StatusCode::Conflict, "Stale control_generation");
        }
        if (entry->desired_state != DesiredState::Stopped || entry->execution ||
            entry->configuring || entry->resetting || entry->retiring) {
            return Status(StatusCode::InvalidState, "Task must be Stopped and idle to Configure");
        }
        auto op_id = task::new_operation_id();
        if (!op_id.ok()) {
            return op_id.status();
        }
        op.operation_id = op_id.value();
        op.kind = OperationKind::Configure;
        op.state = OperationState::InProgress;
        op.control_generation = entry->control_generation + 1;
        op.accepted_at = now;
        base_meta = entry->metadata;
        entry->configuring = true;
    }

    struct ConfigureJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        task::TaskMetadata base_meta;
        TaskConfig config;
        RequestIdentity request;
        task::RequestSignature sig;
        OperationSnapshot op;
        std::shared_ptr<task::Reply<OperationId>> reply;

        void run() noexcept override {
            Result<task::LoadedTask> updated(Status(StatusCode::Internal, {}));
            try {
                std::lock_guard<std::mutex> slock(owner->store_mutex);
                updated = owner->store->update_config(base_meta, config);
            } catch (const std::bad_alloc&) {
                updated = Status(StatusCode::ResourceExhausted, {});
            } catch (...) {
                updated = Status(StatusCode::Internal, {});
            }
            const auto finished_at = task::control_wall_now();
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->configuring = false;
            if (!updated.ok()) {
                reply->complete(updated.status());
                return;
            }
            entry->metadata = std::move(updated.value().metadata);
            entry->config =
                std::make_shared<const TaskConfig>(std::move(updated.value().active_config));
            entry->control_generation = op.control_generation;
            op.state = OperationState::Succeeded;
            op.terminal_at = finished_at;
            op.status_code = StatusCode::Ok;
            entry->current_operation = op;
            (void)entry->operations.record(request.request_key, std::move(sig), op, finished_at);
            owner->publish_locked(*entry);
            reply->complete(op.operation_id);
        }
    };

    try {
        auto job = std::make_shared<ConfigureJob>();
        job->owner = state_;
        job->entry = entry;
        job->base_meta = std::move(base_meta);
        job->config = config;
        job->request = request;
        job->sig = sig;
        job->op = op;
        job->reply = std::make_shared<task::Reply<OperationId>>();
        auto reply = job->reply;
        auto submitted = state_->options->control_lane->submit(std::move(job));
        if (!submitted.ok()) {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->configuring = false;
            return submitted.status();
        }
        return reply->wait(state_->options->request_wait);
    } catch (const std::bad_alloc&) {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        entry->configuring = false;
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationId> TaskService::lifecycle(
    TaskId task_id, OperationKind kind, const RequestIdentity& request,
    const ControlPrecondition& precondition) {
    auto slot = state_->enter_request();
    if (!slot.ok()) {
        return slot.status();
    }
    if (!uuid::nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto valid_req = task::validate_request_identity(request);
    if (!valid_req.ok()) {
        return valid_req.status();
    }
    auto entry = state_->find(task_id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }

    const task::RequestSignature sig{kind, precondition, std::nullopt};
    const auto now = task::control_wall_now();

    std::shared_ptr<task::RunExecution> to_prepare;
    std::shared_ptr<task::RunExecution> to_close;
    bool launch_reset = false;
    bool launch_retire = false;
    OperationId accepted_op_id;
    ControlGeneration accepted_gen = 0;

    {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        if (entry->retired) {
            return Status(StatusCode::NotFound, {});
        }
        auto dup = entry->operations.find_request(request.request_key, sig, now);
        if (dup.ok()) {
            return dup.value();
        }
        if (dup.status().code() != StatusCode::NotFound) {
            return dup.status();
        }
        if (precondition.has_expected_control_generation &&
            precondition.expected_control_generation != entry->control_generation) {
            return Status(StatusCode::Conflict, "Stale control_generation");
        }
        if (entry->configuring || entry->retiring) {
            return Status(StatusCode::Conflict, "Task control mutation in flight");
        }
        if (kind != OperationKind::Stop && entry->resetting) {
            return Status(StatusCode::Conflict, "Task reset in flight");
        }

        // Validate state machine transition.
        switch (kind) {
            case OperationKind::Start:
                if (entry->desired_state != DesiredState::Stopped || entry->execution ||
                    entry->protected_state) {
                    return Status(StatusCode::InvalidState, "Start requires Stopped idle Task");
                }
                break;
            case OperationKind::Pause:
                if (entry->desired_state != DesiredState::Running || !entry->execution) {
                    return Status(StatusCode::InvalidState, "Pause requires Running Task");
                }
                break;
            case OperationKind::Resume:
                if (entry->desired_state != DesiredState::Paused || entry->execution ||
                    entry->protected_state) {
                    return Status(StatusCode::InvalidState, "Resume requires Paused settled Task");
                }
                break;
            case OperationKind::Stop:
                if (entry->desired_state == DesiredState::Stopped && !entry->execution) {
                    return Status(StatusCode::InvalidState, "Task is already Stopped");
                }
                break;
            case OperationKind::Reset:
                if (entry->desired_state != DesiredState::Stopped || entry->execution) {
                    return Status(StatusCode::InvalidState, "Reset requires Stopped idle Task");
                }
                break;
            case OperationKind::Retire:
                if (entry->desired_state != DesiredState::Stopped || entry->execution) {
                    return Status(StatusCode::InvalidState, "Retire requires Stopped idle Task");
                }
                break;
            case OperationKind::Configure:
                return Status(StatusCode::InvalidArgument, {});
        }

        auto op_id = task::new_operation_id();
        if (!op_id.ok()) {
            return op_id.status();
        }
        accepted_op_id = op_id.value();
        accepted_gen = ++entry->control_generation;

        // Supersede any non-terminal current operation when Stop/Pause preempts it.
        if (entry->current_operation && !task::is_terminal(entry->current_operation->state)) {
            entry->current_operation->state = OperationState::Superseded;
            entry->current_operation->terminal_at = now;
            entry->operations.update(*entry->current_operation);
        }

        OperationSnapshot op;
        op.operation_id = accepted_op_id;
        op.kind = kind;
        op.state = OperationState::InProgress;
        op.control_generation = accepted_gen;
        op.accepted_at = now;

        if (kind == OperationKind::Start || kind == OperationKind::Resume) {
            if (!state_->options->execution || !state_->options->execution->resolve) {
                --entry->control_generation;
                return Status(StatusCode::Unavailable, "Execution backend unavailable");
            }
            if (kind == OperationKind::Start) {
                entry->recording_base_ns = 0;
            }
            entry->desired_state = DesiredState::Running;
            auto exec = std::make_shared<task::RunExecution>();
            exec->task_id = entry->id;
            exec->config_version = entry->metadata.active_config_version;
            exec->control_generation = accepted_gen;
            exec->start_operation_id = accepted_op_id;
            exec->recording_base_ns = entry->recording_base_ns;
            exec->config = entry->config;
            entry->execution = exec;
            to_prepare = exec;
        } else if (kind == OperationKind::Pause) {
            entry->desired_state = DesiredState::Paused;
            to_close = entry->execution;
            to_close->fence();
        } else if (kind == OperationKind::Stop) {
            entry->desired_state = DesiredState::Stopped;
            if (entry->execution) {
                to_close = entry->execution;
                to_close->fence();
            } else {
                entry->recording_base_ns = 0;
                op.state = OperationState::Succeeded;
                op.terminal_at = now;
            }
        } else if (kind == OperationKind::Reset) {
            entry->resetting = true;
            launch_reset = true;
        } else if (kind == OperationKind::Retire) {
            entry->retiring = true;
            launch_retire = true;
        }

        auto recorded = entry->operations.record(request.request_key, sig, op, now);
        if (!recorded.ok()) {
            // Roll back in-memory mutation if registry full.
            if (to_prepare) {
                entry->execution.reset();
                entry->desired_state =
                    kind == OperationKind::Resume ? DesiredState::Paused : DesiredState::Stopped;
            }
            if (launch_reset) {
                entry->resetting = false;
            }
            if (launch_retire) {
                entry->retiring = false;
            }
            --entry->control_generation;
            return recorded.status();
        }
        entry->current_operation = op;
        state_->publish_locked(*entry);
    }

    if (to_prepare) {
        state_->start_prepare(entry, to_prepare);
    } else if (to_close) {
        state_->start_close(
            entry, to_close, accepted_op_id, accepted_gen, kind == OperationKind::Pause, false,
            false);
    } else if (launch_reset) {
        state_->start_reset(entry, accepted_op_id, accepted_gen);
    } else if (launch_retire) {
        state_->start_retire(entry, accepted_op_id, accepted_gen);
    }

    return accepted_op_id;
}

Result<OperationId> TaskService::start(
    TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(task_id, OperationKind::Start, request, precondition);
}

Result<OperationId> TaskService::pause(
    TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(task_id, OperationKind::Pause, request, precondition);
}

Result<OperationId> TaskService::resume(
    TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(task_id, OperationKind::Resume, request, precondition);
}

Result<OperationId> TaskService::stop(
    TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(task_id, OperationKind::Stop, request, precondition);
}

Result<OperationId> TaskService::reset(
    TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(task_id, OperationKind::Reset, request, precondition);
}

Result<OperationId> TaskService::retire(
    TaskId task_id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(task_id, OperationKind::Retire, request, precondition);
}

Result<TaskSnapshot> TaskService::get_task(TaskId task_id) const {
    if (!uuid::nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto entry = state_->find(task_id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    const auto pub = std::atomic_load(&entry->published);
    if (!pub) {
        return Status(StatusCode::Unavailable, {});
    }
    auto snap = pub->snapshot;
    if (snap.freshness != SnapshotFreshness::NotObserved) {
        auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - pub->observed_at);
        if (age < std::chrono::nanoseconds::zero()) {
            age = std::chrono::nanoseconds::zero();
        }
        snap.age = age;
        if (snap.freshness == SnapshotFreshness::Fresh &&
            age > state_->options->runtime_snapshot_max_age) {
            snap.freshness = SnapshotFreshness::Stale;
        }
    }
    return snap;
}

Result<TaskRuntimeSnapshot> TaskService::get_runtime_snapshot(TaskId task_id) const {
    if (!uuid::nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto entry = state_->find(task_id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    const auto pub = std::atomic_load(&entry->published);
    if (!pub) {
        return Status(StatusCode::Unavailable, {});
    }
    auto runtime = pub->runtime;
    if (runtime.freshness != SnapshotFreshness::NotObserved) {
        auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - pub->observed_at);
        if (age < std::chrono::nanoseconds::zero()) {
            age = std::chrono::nanoseconds::zero();
        }
        runtime.age = age;
        if (runtime.freshness == SnapshotFreshness::Fresh &&
            age > state_->options->runtime_snapshot_max_age) {
            runtime.freshness = SnapshotFreshness::Stale;
        }
    }
    return runtime;
}

Result<TaskConfig> TaskService::get_active_config(TaskId task_id) const {
    if (!uuid::nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto entry = state_->find(task_id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    const auto pub = std::atomic_load(&entry->published);
    if (!pub || !pub->config) {
        return Status(StatusCode::Unavailable, {});
    }
    return *pub->config;
}

Result<TaskPage> TaskService::list_tasks(const PageRequest& request) const {
    if (request.limit == 0 || request.limit > 256 || request.cursor.size() > 256) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::optional<TaskId> after_id;
    if (!request.cursor.empty()) {
        constexpr std::string_view kPrefix = "tk1:";
        if (request.cursor.size() <= kPrefix.size() ||
            std::string_view(request.cursor).substr(0, kPrefix.size()) != kPrefix) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto parsed = parse_task_id(std::string_view(request.cursor).substr(kPrefix.size()));
        if (!parsed.ok()) {
            return Status(StatusCode::InvalidArgument, {});
        }
        after_id = parsed.value();
    }
    try {
        std::vector<std::shared_ptr<task::TaskEntry>> snapshot_entries;
        {
            std::lock_guard<std::mutex> lock(state_->catalog_mutex);
            snapshot_entries = state_->entries;
        }
        std::sort(
            snapshot_entries.begin(), snapshot_entries.end(),
            [](const auto& a, const auto& b) { return a->id < b->id; });

        std::vector<TaskSummary> matched;
        matched.reserve(snapshot_entries.size());
        for (const auto& entry : snapshot_entries) {
            if (after_id && !(*after_id < entry->id)) {
                continue;
            }
            const auto pub = std::atomic_load(&entry->published);
            if (!pub) {
                continue;
            }
            matched.push_back(build_summary(*pub, state_->options->runtime_snapshot_max_age));
        }

        TaskPage page;
        const std::size_t count = std::min<std::size_t>(matched.size(), request.limit);
        page.items.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            page.items.push_back(std::move(matched[i]));
        }
        page.has_more = matched.size() > count;
        if (page.has_more && count > 0) {
            page.next_cursor = "tk1:" + to_string(page.items.back().task.task_id);
        }
        return page;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationSnapshot> TaskService::get_operation(
    TaskId task_id, OperationId operation_id) const {
    if (!uuid::nonzero(task_id.value) || !uuid::nonzero(operation_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto entry = state_->find(task_id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    std::lock_guard<std::mutex> lock(entry->control_mutex);
    return entry->operations.get(operation_id, task::control_wall_now());
}

Result<OperationPage> TaskService::list_operations(
    TaskId task_id, const PageRequest& request) const {
    if (!uuid::nonzero(task_id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto entry = state_->find(task_id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    std::lock_guard<std::mutex> lock(entry->control_mutex);
    return entry->operations.list(task_id, request, task::control_wall_now());
}

}  // namespace rodaq
