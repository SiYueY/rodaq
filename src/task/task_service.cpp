#include "task_service_state.hpp"

#include "config.hpp"
#include "config_identity.hpp"
#include "create_store.hpp"
#include "identity.hpp"
#include "task_controller.hpp"
#include "../pipeline/stream_identity_store.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <set>

#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/version.hpp>

namespace rodaq {
namespace {

namespace fs = std::filesystem;
using task::PublishedTask;
using task::Reply;
using task::RequestSlot;
using task::TaskEntry;

bool label(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 128 &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return byte >= 0x21 && byte <= 0x7e;
           });
}

}  // namespace

Result<std::shared_ptr<const PublishedTask>> task::make_task_publication(
    const TaskEntry& entry, std::shared_ptr<const TaskConfig> config, std::uint64_t sequence,
    std::shared_ptr<const task::TaskControlRecord> control) {
    try {
        if (!control) {
            control = entry.controller->durable_record();
        }
        TaskSnapshot snapshot;
        snapshot.task_id = entry.id;
        snapshot.active_config_version = control->active_config_version;
        snapshot.desired_state = control->desired_state;
        snapshot.control_generation = control->control_generation;
        snapshot.reset_generation = control->reset_generation;
        snapshot.snapshot_sequence = sequence;
        snapshot.captured_at = task::control_wall_now();
        snapshot.execution_condition =
            entry.quiescent && control->desired_state != DesiredState::Running
                ? ExecutionCondition::Idle
                : ExecutionCondition::Protected;
        if (control->reset_intent || entry.resetting) {
            snapshot.execution_condition = ExecutionCondition::Draining;
        }
        if (entry.execution) {
            snapshot.execution_condition = entry.execution->condition(control->desired_state);
        }
        if (control->current_operation_id) {
            for (const auto& operation : control->operation_history.entries) {
                if (operation.snapshot.operation_id == *control->current_operation_id) {
                    snapshot.current_operation = operation.snapshot;
                }
            }
        }
        auto runtime = capture_task_runtime(entry, *config, snapshot);
        if (!runtime.ok()) {
            return runtime.status();
        }
        snapshot.data_impact = runtime.value().data_impact;
        auto published = std::make_shared<PublishedTask>(PublishedTask{
            std::move(snapshot), std::move(control), std::move(config), std::move(runtime.value()),
            std::chrono::steady_clock::now()});
        const auto previous = std::atomic_load(&entry.published);
        project_runtime_rates(*published, previous.get());
        return std::shared_ptr<const PublishedTask>(std::move(published));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

persistence::FileFaultHook TaskService::State::file_hook(task::ServiceFileDomain domain) const {
    return [policy = options, domain](auto step) {
        return policy->file_faults ? policy->file_faults(domain, step) : 0;
    };
}

Result<void> TaskService::State::step(task::MaterializationStep value) const {
    if (options->materialization_faults && options->materialization_faults(value) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    return {};
}

Result<void> TaskService::State::directory(const fs::path& path) const {
    std::error_code error;
    fs::create_directory(path, error);
    if (error || fs::symlink_status(path).type() != fs::file_type::directory) {
        return Status(StatusCode::Unavailable, {});
    }
    return persistence::sync_directory(
        path.parent_path(), file_hook(task::ServiceFileDomain::Directories));
}

Result<void> TaskService::State::validate_deployment(const TaskConfig& config) const {
    for (const auto& pipeline : config.pipelines) {
        const auto profile = std::find_if(
            options->resource_profiles.begin(), options->resource_profiles.end(),
            [&](const auto& admitted) { return admitted.name == pipeline.resource_profile; });
        if (profile == options->resource_profiles.end() ||
            std::find(
                options->output_roots.begin(), options->output_roots.end(), pipeline.output_root) ==
                options->output_roots.end()) {
            return Status(StatusCode::PermissionDenied, {});
        }
        if (config.segment_policy.max_physical_bytes &&
            *config.segment_policy.max_physical_bytes > profile->max_segment_physical_bytes) {
            return Status(StatusCode::InvalidArgument, {});
        }
        const auto allowed = [&](const PluginConfig& selected, task::AdmissionRole role) {
            const bool installed_exact =
                std::any_of(installed.begin(), installed.end(), [&](const auto& plugin) {
                    return plugin.plugin_id == selected.plugin_id &&
                           plugin.plugin_version == selected.plugin_version;
                });
            return installed_exact &&
                   std::any_of(
                       options->allowed_plugins.begin(), options->allowed_plugins.end(),
                       [&](const auto& plugin) {
                           return plugin.plugin_id == selected.plugin_id &&
                                  plugin.plugin_version == selected.plugin_version &&
                                  plugin.role == role;
                       });
        };
        if (!allowed(pipeline.acquisition, task::AdmissionRole::Acquisition) ||
            !allowed(pipeline.storage, task::AdmissionRole::Storage)) {
            return Status(StatusCode::PermissionDenied, {});
        }
    }
    return {};
}

std::shared_ptr<TaskEntry> TaskService::State::find(TaskId id) {
    std::lock_guard<std::mutex> lock(catalog_mutex);
    const auto found = std::lower_bound(
        entries.begin(), entries.end(), id,
        [](const auto& entry, TaskId key) { return entry->id < key; });
    return found != entries.end() && (*found)->id == id ? *found : nullptr;
}

Result<void> TaskService::State::insert(std::shared_ptr<TaskEntry> entry) {
    std::lock_guard<std::mutex> lock(catalog_mutex);
    const auto position = std::lower_bound(
        entries.begin(), entries.end(), entry->id,
        [](const auto& current, TaskId key) { return current->id < key; });
    if (position != entries.end() && (*position)->id == entry->id) {
        return {};
    }
    if (entries.size() == options->max_tasks) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    entries.insert(position, std::move(entry));  // Capacity was reserved at startup.
    return {};
}

Result<std::shared_ptr<TaskEntry>> TaskService::State::materialize(
    TaskId id, const task::AssignedConfig* initial) {
    const auto task_directory = options->root / "tasks" / to_string(id);
    if (initial) {
        auto made = directory(task_directory);
        if (!made.ok()) {
            return made.status();
        }
        made = directory(task_directory / "configs");
        if (!made.ok()) {
            return made.status();
        }
    }
    auto store = task::TaskStore::open(
        task_directory, options->root / "control", id,
        initial ? task::TaskStoreMode::Materializing : task::TaskStoreMode::Existing,
        options->store, file_hook(task::ServiceFileDomain::Task));
    if (!store.ok()) {
        return store.status();
    }
    auto identities = pipeline::StreamIdentityStore::open(
        options->root / "identities", id,
        initial ? pipeline::IdentityStoreMode::Materializing
                : pipeline::IdentityStoreMode::Existing,
        options->max_stream_identities, file_hook(task::ServiceFileDomain::StreamIdentity));
    if (!identities.ok()) {
        return identities.status();
    }
    if (initial) {
        auto created = store.value()->create_config(initial->config);
        if (!created.ok()) {
            return created.status();
        }
        auto checked = step(task::MaterializationStep::Config);
        if (!checked.ok()) {
            return checked.status();
        }
        auto projection = task::identity_projection(*initial, options->store.config);
        if (!projection.ok()) {
            return projection.status();
        }
        auto initialized = identities.value()->initialize(projection.value());
        if (!initialized.ok()) {
            return initialized.status();
        }
        checked = step(task::MaterializationStep::Identity);
        if (!checked.ok()) {
            return checked.status();
        }
        task::TaskControlRecord candidate;
        candidate.task_id = id;
        candidate.active_config_version = created.value().version;
        auto actual = store.value()->load_control();
        if (!actual.ok() && actual.status().code() == StatusCode::NotFound) {
            auto published = store.value()->publish_control(candidate, 0);
            if (!published.ok()) {
                return published.status();
            }
        } else if (!actual.ok()) {
            return actual.status();
        } else {
            auto old = task::encode_control_record(actual.value(), options->store.history);
            auto expected = task::encode_control_record(candidate, options->store.history);
            if (!old.ok() || !expected.ok() || old.value() != expected.value()) {
                return Status(
                    StatusCode::DataLoss, "Half-created Task has foreign Control authority");
            }
        }
        checked = step(task::MaterializationStep::Control);
        if (!checked.ok()) {
            return checked.status();
        }
    }
    auto control = store.value()->load_control();
    if (!control.ok()) {
        return control.status();
    }
    auto config = store.value()->load_config(control.value().active_config_version);
    if (!config.ok()) {
        return config.status();
    }
    auto identity = identities.value()->load();
    if (!identity.ok()) {
        return identity.status();
    }
    task::AssignedConfig selected{
        config.value().config, identity.value().pipeline_id_high_water,
        identity.value().stream_id_high_water};
    auto projection = task::identity_projection(selected, options->store.config);
    if (!projection.ok()) {
        return projection.status();
    }
    auto applied = identities.value()->apply_config_change(
        projection.value(), identity.value().record_revision);
    if (!applied.ok()) {
        return applied.status();
    }
    identity = identities.value()->load();
    if (!identity.ok()) {
        return identity.status();
    }
    auto controller = task::TaskController::create(control.value(), options->store.history);
    if (!controller.ok()) {
        return controller.status();
    }
    auto entry = std::make_shared<TaskEntry>();
    entry->id = id;
    entry->store = std::move(store.value());
    entry->identities = std::move(identities.value());
    entry->identity = std::move(identity.value());
    entry->controller = std::move(controller.value());
    if (options->execution) {
        auto recovery = task::TaskRecoveryStore::open(
            options->root / "recovery", id, options->execution->recovery,
            file_hook(task::ServiceFileDomain::Recovery));
        if (!recovery.ok()) {
            return recovery.status();
        }
        entry->recovery = std::move(recovery.value());
    }
    const auto& history = control.value().operation_history;
    // Configure is accepted only after all prior execution owners are quiescent.
    // The current operation is retained even when older history has expired.
    const bool configuration_boundary =
        std::any_of(history.entries.begin(), history.entries.end(), [&](const auto& operation) {
            return control.value().current_operation_id == operation.snapshot.operation_id &&
                   operation.snapshot.kind == OperationKind::Configure;
        });
    const bool never_started =
        !history.history_truncated &&
        std::none_of(history.entries.begin(), history.entries.end(), [](const auto& operation) {
            return operation.snapshot.kind == OperationKind::Start ||
                   operation.snapshot.kind == OperationKind::Resume;
        });
    const auto* current_op = [&]() -> const task::StoredOperation* {
        if (!control.value().current_operation_id) {
            return nullptr;
        }
        for (const auto& op : history.entries) {
            if (op.snapshot.operation_id == *control.value().current_operation_id) {
                return &op;
            }
        }
        return nullptr;
    }();
    entry->quiescent = control.value().desired_state == DesiredState::Stopped &&
                       !control.value().reset_intent && (configuration_boundary || never_started);
    if (control.value().reset_intent) {
        entry->quiescent = false;
        entry->boot_action = task::BootAction::ResumeReset;
    } else if (control.value().desired_state == DesiredState::Running) {
        entry->quiescent = true;
        if (current_op && !task::terminal(current_op->snapshot.state) &&
            (current_op->snapshot.kind == OperationKind::Start ||
             current_op->snapshot.kind == OperationKind::Resume)) {
            entry->boot_action = task::BootAction::ResumeRunOperation;
        } else if (
            current_op &&
            (current_op->snapshot.state == OperationState::Succeeded ||
             current_op->snapshot.state == OperationState::PartiallySucceeded) &&
            (current_op->snapshot.kind == OperationKind::Start ||
             current_op->snapshot.kind == OperationKind::Resume) &&
            config.value().config.recovery_policy == RecoveryPolicyMode::Resume &&
            validate_deployment(config.value().config).ok()) {
            entry->boot_action = task::BootAction::AutoResumeInterrupted;
        }
    } else if (
        current_op && !task::terminal(current_op->snapshot.state) &&
        (current_op->snapshot.kind == OperationKind::Pause ||
         current_op->snapshot.kind == OperationKind::Stop)) {
        entry->quiescent = false;
        entry->boot_action = task::BootAction::SettleInterruptedClose;
    } else if (control.value().desired_state == DesiredState::Paused) {
        entry->quiescent = true;
    } else if (
        !entry->quiescent && entry->recovery &&
        control.value().desired_state == DesiredState::Stopped && !control.value().reset_intent) {
        const bool stopped = current_op != nullptr &&
                             current_op->snapshot.kind == OperationKind::Stop &&
                             current_op->snapshot.state == OperationState::Succeeded;
        auto recovered = entry->recovery->load();
        if (stopped) {
            if (!recovered.ok()) {
                return Status(StatusCode::Unavailable, {});
            }
            const auto& record = recovered.value();
            entry->quiescent =
                record.observed_control_generation == control.value().control_generation &&
                record.active_config_version == control.value().active_config_version &&
                !record.reset_progress && record.cleanup_intents.empty() && record.run_operation &&
                record.run_operation->phase == task::RunPhase::Ended &&
                std::all_of(
                    record.pipelines.begin(), record.pipelines.end(),
                    [](const auto& recent) { return recent.instance.ended_boundary.has_value(); });
            if (!entry->quiescent) {
                return Status(StatusCode::Unavailable, {});
            }
        }
    }
    auto active = std::make_shared<const TaskConfig>(std::move(config.value().config));
    if (entry->quiescent) {
        auto terminal = task::completed_configuration(*entry->controller->durable_record());
        if (terminal) {
            auto work = entry->controller->update_operation(*terminal);
            if (!work.ok()) {
                return work.status();
            }
            auto actual = task::execute_control_work(
                *entry->store, *work.value(), entry->identities.get(), options->store.config);
            auto completed = entry->controller->persistence_completed(
                actual.work_id, actual.authority, actual.publication_failure);
            if (!completed.ok()) {
                return completed.status();
            }
            if (!completed.value().resolved || !completed.value().resolved->durable) {
                return completed.value().resolved ? completed.value().resolved->status
                                                  : Status(StatusCode::DataLoss, {});
            }
            entry->identity = std::move(*actual.identities);
            active = std::move(actual.active_config);
        }
    }
    auto published = task::make_task_publication(*entry, std::move(active), 1);
    if (!published.ok()) {
        return published.status();
    }
    entry->published = std::move(published.value());
    return entry;
}

Result<TaskId> TaskService::State::create(
    const TaskConfig& config, const RequestIdentity& request) {
    if (reconcile_creates) {
        auto reconciled = creates->reconcile();
        if (!reconciled.ok()) {
            return reconciled.status();
        }
        reconcile_creates = false;
    }
    auto digest = task::create_request_digest(config, options->store.config);
    auto key = task::request_key_hash(request);
    if (!digest.ok()) {
        return digest.status();
    }
    if (!key.ok()) {
        return key.status();
    }
    auto known = creates->lookup(key.value(), digest.value());
    if (!known.ok()) {
        reconcile_creates = true;
        return known.status();
    }
    if (!known.value() || known.value()->phase != task::CreatePhase::Completed) {
        auto allowed = validate_deployment(config);
        if (!allowed.ok()) {
            return allowed.status();
        }
        auto retained = creates->retained_receipts();
        if (!retained.ok()) {
            reconcile_creates = true;
            return retained.status();
        }
        // Completed authority can exist before ordinary catalog publication,
        // including after an uncertain Completed write. It still owns a Task
        // slot. Never admit another Task merely because that entry is invisible.
        const auto completed = std::count_if(
            retained.value().begin(), retained.value().end(), [&](const auto& receipt) {
                return receipt.phase == task::CreatePhase::Completed &&
                       (!retirements || !retirements->is_retired(receipt.assigned_task_id));
            });
        if (static_cast<std::size_t>(completed) >= options->max_tasks) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    }
    if (!known.value()) {
        auto candidate = task::new_task_id();
        if (!candidate.ok()) {
            return candidate.status();
        }
        auto claimed = creates->claim(
            key.value(), digest.value(), candidate.value(), task::control_wall_now());
        if (!claimed.ok()) {
            reconcile_creates = true;
            return claimed.status();
        }
        known.value() = std::move(claimed.value().receipt);
    }
    auto receipt = std::move(*known.value());
    if (receipt.phase == task::CreatePhase::Completed) {
        if (!find(receipt.assigned_task_id)) {
            auto entry = materialize(receipt.assigned_task_id, nullptr);
            if (!entry.ok()) {
                return entry.status();
            }
            auto added = insert(std::move(entry.value()));
            if (!added.ok()) {
                return added.status();
            }
        }
        return receipt.assigned_task_id;
    }
    auto checked = step(task::MaterializationStep::Allocated);
    if (!checked.ok()) {
        return checked.status();
    }
    if (receipt.phase == task::CreatePhase::Allocated) {
        receipt.phase = task::CreatePhase::Materializing;
        auto published = creates->publish(receipt);
        if (!published.ok()) {
            reconcile_creates = true;
            return published.status();
        }
    }
    checked = step(task::MaterializationStep::Materializing);
    if (!checked.ok()) {
        return checked.status();
    }
    auto assigned = task::assign_initial_config(config, options->store.config);
    if (!assigned.ok()) {
        return assigned.status();
    }
    auto entry = materialize(receipt.assigned_task_id, &assigned.value());
    if (!entry.ok()) {
        return entry.status();
    }
    receipt.phase = task::CreatePhase::Completed;
    auto published = creates->publish(receipt);
    if (!published.ok()) {
        reconcile_creates = true;
        return published.status();
    }
    auto added = insert(std::move(entry.value()));
    if (!added.ok()) {
        return added.status();
    }
    checked = step(task::MaterializationStep::Completed);
    if (!checked.ok()) {
        return checked.status();
    }
    return receipt.assigned_task_id;
}

void TaskService::State::CreateWork::run() noexcept {
    try {
        reply->complete(owner->create(config, request));
    } catch (const std::bad_alloc&) {
        reply->complete(Status(StatusCode::ResourceExhausted, {}));
    } catch (const fs::filesystem_error&) {
        reply->complete(Status(StatusCode::Unavailable, {}));
    } catch (...) {
        reply->complete(Status(StatusCode::Internal, {}));
    }
}

TaskService::TaskService(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

TaskService::~TaskService() noexcept { state_->accepting.store(false, std::memory_order_release); }

Result<std::unique_ptr<TaskService>> TaskService::open(task::ServiceOptions options) {
    if (!options.plugin_manager || !options.create_lane || !options.control_lane ||
        options.create_lane == options.control_lane || options.max_tasks == 0 ||
        options.max_tasks > 1024 || options.max_requests == 0 || options.max_requests > 4096 ||
        options.create_recovery_slots > 1024 || options.max_stream_identities == 0 ||
        options.max_stream_identities > 65536 ||
        options.request_wait <= std::chrono::nanoseconds::zero() ||
        options.request_wait > std::chrono::hours{24} || options.resource_profiles.empty() ||
        options.runtime_snapshot_max_age <= std::chrono::nanoseconds::zero() ||
        options.runtime_snapshot_max_age > std::chrono::hours{24} || options.output_roots.empty() ||
        options.allowed_plugins.empty()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const auto lane = options.create_lane->snapshot();
        const auto control_lane = options.control_lane->snapshot();
        if (lane.worker_count != 1 || !lane.accepting || !control_lane.accepting ||
            control_lane.capacity < options.max_tasks ||
            fs::symlink_status(options.root).type() != fs::file_type::directory) {
            return Status(StatusCode::InvalidArgument, {});
        }
        if (options.execution) {
            const auto& execution = *options.execution;
            if (!execution.sample || !execution.resolve || !execution.commit ||
                !execution.reserve_segment || !execution.preparation_lane ||
                !execution.cleanup_lane || execution.preparation_lane == execution.cleanup_lane ||
                execution.preparation_lane == options.control_lane ||
                execution.preparation_lane == options.create_lane ||
                execution.cleanup_lane == options.control_lane ||
                execution.cleanup_lane == options.create_lane ||
                execution.native_timeout <= std::chrono::nanoseconds::zero() ||
                execution.native_timeout > std::chrono::hours{1} ||
                !task::valid_runtime_recovery_policy(execution.runtime_recovery)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            for (const auto& domain : {execution.preparation_lane, execution.cleanup_lane}) {
                const auto capacity = domain->snapshot();
                if (!capacity.accepting || capacity.worker_count < options.max_tasks ||
                    capacity.capacity < options.max_tasks) {
                    return Status(StatusCode::InvalidArgument, {});
                }
            }
        }
        const auto valid_labels = [](const auto& values) {
            if (values.size() > 256) {
                return false;
            }
            std::set<std::string_view> seen;
            for (const auto& value : values) {
                if (!label(value) || !seen.insert(value).second) {
                    return false;
                }
            }
            return true;
        };
        if (options.resource_profiles.size() > 256 || !valid_labels(options.output_roots) ||
            options.allowed_plugins.size() > 512 || options.store.max_config_versions == 0 ||
            options.store.max_config_versions > 4096 ||
            !task::validate_history({}, options.store.history).ok()) {
            return Status(StatusCode::InvalidArgument, {});
        }
        std::set<std::string_view> profile_names;
        for (const auto& profile : options.resource_profiles) {
            if (!label(profile.name) || profile.max_segment_physical_bytes == 0 ||
                !profile_names.insert(profile.name).second) {
                return Status(StatusCode::InvalidArgument, {});
            }
        }
        for (std::size_t i = 0; i < options.allowed_plugins.size(); ++i) {
            const auto& pin = options.allowed_plugins[i];
            if (!plugin::abi::valid_identifier(pin.plugin_id) || pin.plugin_version.size() > 128 ||
                !plugin::validate_plugin_version(pin.plugin_version).ok() ||
                (pin.role != task::AdmissionRole::Acquisition &&
                 pin.role != task::AdmissionRole::Storage) ||
                std::any_of(
                    options.allowed_plugins.begin(),
                    options.allowed_plugins.begin() + static_cast<std::ptrdiff_t>(i),
                    [&](const auto& old) {
                        return old.plugin_id == pin.plugin_id &&
                               old.plugin_version == pin.plugin_version && old.role == pin.role;
                    })) {
                return Status(StatusCode::InvalidArgument, {});
            }
        }
        auto installed = options.plugin_manager->plugins();
        if (!installed.ok()) {
            return installed.status();
        }
        auto state = std::make_shared<State>();
        state->options = std::make_shared<const task::ServiceOptions>(std::move(options));
        state->installed = std::move(installed.value());
        state->entries.reserve(state->options->max_tasks);
        for (const auto* name :
             {"tasks", "control", "identities", "create", "recovery", "retired"}) {
            auto made = state->directory(state->options->root / name);
            if (!made.ok()) {
                return made.status();
            }
        }
        auto retirements = task::TaskRetirementStore::open(
            state->options->root / "retired", 4096,
            state->file_hook(task::ServiceFileDomain::CreateReceipts));
        if (!retirements.ok()) {
            return retirements.status();
        }
        state->retirements = std::move(retirements.value());
        auto creates = task::TaskCreateStore::open(
            state->options->root / "create",
            state->options->max_tasks + state->options->create_recovery_slots,
            state->file_hook(task::ServiceFileDomain::CreateReceipts));
        if (!creates.ok()) {
            return creates.status();
        }
        state->creates = std::move(creates.value());
        auto receipts = state->creates->retained_receipts();
        if (!receipts.ok()) {
            return receipts.status();
        }
        for (const auto& receipt : receipts.value()) {
            if (receipt.phase == task::CreatePhase::Completed &&
                !state->retirements->is_retired(receipt.assigned_task_id)) {
                auto entry = state->materialize(receipt.assigned_task_id, nullptr);
                if (!entry.ok()) {
                    return entry.status();
                }
                auto added = state->insert(std::move(entry.value()));
                if (!added.ok()) {
                    return added.status();
                }
            }
        }
        return std::unique_ptr<TaskService>(new TaskService(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const fs::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<TaskId> TaskService::create_task(const TaskConfig& config, const RequestIdentity& request) {
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, {});
    }
    auto count = state_->requests.load(std::memory_order_relaxed);
    do {
        if (count == state_->options->max_requests) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    } while (!state_->requests.compare_exchange_weak(count, count + 1, std::memory_order_acquire));
    RequestSlot slot(state_->requests);
    try {
        auto normalized = task::normalize_config(config, state_->options->store.config);
        if (!normalized.ok()) {
            return normalized.status();
        }
        auto valid_request = task::validate_request_identity(request);
        if (!valid_request.ok()) {
            return valid_request.status();
        }
        auto reply = std::make_shared<Reply<TaskId>>();
        auto work = std::make_shared<State::CreateWork>();
        work->owner = state_;
        work->config = std::move(normalized.value());
        work->request = request;
        work->reply = reply;
        auto submitted = state_->options->create_lane->submit(std::move(work));
        if (!submitted.ok()) {
            return submitted.status();
        }
        return reply->wait(state_->options->request_wait);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
