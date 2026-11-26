#include "task_service_state.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq::task {

void RunExecution::fence(std::optional<std::uint64_t> monotonic_ns) noexcept {
    std::lock_guard<std::mutex> lock(registration);
    fenced = true;
    if (authorization) {
        (void)authorization->fence(monotonic_ns);
    }
}

bool RunExecution::resources_settled() {
    std::lock_guard<std::mutex> lock(registration);
    if (!preparation_returned || preparing || rotating) {
        return false;
    }
    for (const auto& selected : pipelines) {
        auto facts = selected.controller->snapshot();
        if (!facts.ok() || !facts.value().quiescence.settled) {
            return false;
        }
    }
    return true;
}

ExecutionCondition RunExecution::condition(DesiredState desired) {
    std::lock_guard<std::mutex> lock(registration);
    bool all_settled = preparation_returned && !rotating;
    bool any_open = false;
    bool all_open = !pipelines.empty();
    bool required_open = true;
    bool failed_pipeline_pending = false;
    bool required_configuration_rejected = false;
    bool recoverable_pipeline = false;
    bool snapshot_unavailable = false;
    for (const auto& selected : pipelines) {
        auto actual = selected.controller->snapshot();
        if (!actual.ok()) {
            snapshot_unavailable = true;
            all_settled = false;
            failed_pipeline_pending =
                failed_pipeline_pending ||
                (selected.prepare_status && *selected.prepare_status != StatusCode::Ok);
            continue;
        }
        all_settled = all_settled && actual.value().quiescence.settled;
        failed_pipeline_pending =
            failed_pipeline_pending ||
            (selected.prepare_status && *selected.prepare_status != StatusCode::Ok &&
             !actual.value().quiescence.settled);
        const bool open = actual.value().gate.open && !actual.value().gate.control_fence &&
                          actual.value().failure == StatusCode::Ok;
        any_open = any_open || open;
        all_open = all_open && open;
        required_open = required_open && (!selected.required || open);
        required_configuration_rejected =
            required_configuration_rejected ||
            (selected.required && actual.value().configuration_rejected);
        recoverable_pipeline = recoverable_pipeline || !actual.value().configuration_rejected;
    }
    if (ended_pending) {
        return ExecutionCondition::PendingCleanup;
    }
    if (failed_pipeline_pending) {
        return ExecutionCondition::PendingCleanup;
    }
    if (draining) {
        return ExecutionCondition::Draining;
    }
    if (preparing) {
        return ExecutionCondition::Preparing;
    }
    if (snapshot_unavailable) {
        return ExecutionCondition::Protected;
    }
    if (any_open) {
        if (!required_open && config->start_policy == StartPolicyMode::AllRequired) {
            return ExecutionCondition::Protected;
        }
        return all_open ? ExecutionCondition::Active : ExecutionCondition::Degraded;
    }
    if (!all_settled) {
        return ExecutionCondition::PendingCleanup;
    }
    if (desired != DesiredState::Running) {
        return ExecutionCondition::Idle;
    }
    // A restart with unchanged immutable inputs cannot repair rejected Plugin
    // configuration. Partial BestEffort runs still take the Degraded path above.
    const bool configuration_blocks_recovery =
        !pipelines.empty() &&
        (!recoverable_pipeline ||
         (config->start_policy == StartPolicyMode::AllRequired && required_configuration_rejected));
    return (configuration_blocks_recovery || recovery_exhausted) ? ExecutionCondition::Failed
                                                                 : ExecutionCondition::Protected;
}

}  // namespace rodaq::task

namespace rodaq {
namespace {

const OperationSnapshot* current_operation(const task::TaskControlRecord& control) noexcept {
    for (const auto& stored : control.operation_history.entries) {
        if (control.current_operation_id &&
            stored.snapshot.operation_id == *control.current_operation_id) {
            return &stored.snapshot;
        }
    }
    return nullptr;
}

std::uint64_t recovery_backoff(const task::RuntimeRecoveryPolicy& policy, std::uint32_t attempt) {
    auto delay = static_cast<std::uint64_t>(policy.initial_backoff.count());
    const auto maximum = static_cast<std::uint64_t>(policy.max_backoff.count());
    for (std::uint32_t i = 1; i < attempt && delay < maximum; ++i) {
        delay = delay > maximum / 2 ? maximum : delay * 2;
    }
    return delay;
}

Result<void> compact_ended_authorization(
    task::TaskEntry& entry, const task::TaskControlRecord& control) {
    auto loaded = entry.recovery->load();
    if (!loaded.ok()) {
        return loaded.status();
    }
    auto record = std::move(loaded.value());
    if (record.observed_control_generation != control.control_generation ||
        record.active_config_version != control.active_config_version || record.reset_progress ||
        !record.cleanup_intents.empty()) {
        return Status(StatusCode::Conflict, {});
    }
    if (!record.run_operation) {
        return {};
    }
    if (!control.current_operation_id ||
        !(record.run_operation->operation_id == *control.current_operation_id) ||
        record.run_operation->phase != task::RunPhase::Ended) {
        return Status(StatusCode::Conflict, {});
    }
    if (record.record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    // Existing Store compaction is legal only after all old instances Ended.
    // Keep recording/committed floors and the immutable user Operation result.
    for (const auto& instance : record.run_operation->authorized_pipelines) {
        const auto recent = std::find_if(
            record.pipelines.begin(), record.pipelines.end(),
            [&](const auto& value) { return value.instance.pipeline_id == instance.pipeline_id; });
        if (recent != record.pipelines.end()) {
            if (!recent->instance.ended_boundary) {
                return Status(StatusCode::Conflict, {});
            }
            *recent = {record.run_operation->control_generation, instance};
        } else {
            record.pipelines.push_back({record.run_operation->control_generation, instance});
        }
    }
    record.run_operation.reset();
    const auto expected = record.record_revision++;
    return entry.recovery->publish(record, expected);
}

}  // namespace

void TaskService::State::start_execution(
    const std::shared_ptr<task::TaskEntry>& entry, task::RunPurpose purpose,
    std::uint64_t now_monotonic) noexcept {
    try {
        auto job = std::make_shared<PrepareJob>();
        auto execution = std::make_shared<task::RunExecution>();
        {
            std::unique_lock<std::mutex> control(entry->control_mutex);
            const auto current = entry->controller->durable_record();
            const auto* operation = current_operation(*current);
            if (!accepting.load(std::memory_order_acquire) || !operation ||
                !entry->controller->run_is_current(
                    operation->operation_id, current->control_generation) ||
                !options->execution ||
                (purpose == task::RunPurpose::Operation && task::terminal(operation->state))) {
                return;
            }
            if (std::atomic_load(&entry->published)->snapshot.snapshot_sequence ==
                std::numeric_limits<std::uint64_t>::max()) {
                return;
            }
            if (purpose == task::RunPurpose::VolumeRecovery) {
                const auto config = std::atomic_load(&entry->published)->config;
                const auto& policy = options->execution->runtime_recovery;
                if (!entry->volume_recovery ||
                    entry->volume_recovery->generation != current->control_generation ||
                    entry->volume_recovery->attempts >= policy.max_attempts ||
                    now_monotonic < entry->volume_recovery->not_before || !entry->quiescent ||
                    entry->persisting || entry->control_work || entry->closing ||
                    entry->cleanup_active_id != 0 || entry->cleanup_ready.frame ||
                    entry->cleanup_pending.frame || !task::terminal(operation->state) ||
                    config->recovery_policy != RecoveryPolicyMode::Resume ||
                    (entry->execution && entry->execution->condition(DesiredState::Running) ==
                                             ExecutionCondition::Failed)) {
                    return;
                }
                auto& retry = *entry->volume_recovery;
                const auto delay = recovery_backoff(policy, ++retry.attempts);
                retry.not_before = delay > std::numeric_limits<std::uint64_t>::max() - now_monotonic
                                       ? std::numeric_limits<std::uint64_t>::max()
                                       : now_monotonic + delay;
            }
            if (purpose == task::RunPurpose::BootRecovery) {
                const auto config = std::atomic_load(&entry->published)->config;
                if (!entry->quiescent || entry->persisting || entry->control_work ||
                    entry->closing || entry->cleanup_active_id != 0 || entry->cleanup_ready.frame ||
                    entry->cleanup_pending.frame || !task::terminal(operation->state) ||
                    config->recovery_policy != RecoveryPolicyMode::Resume) {
                    return;
                }
            }
            if (purpose == task::RunPurpose::Operation && entry->execution &&
                entry->execution->control->current_operation_id == current->current_operation_id) {
                const auto existing = entry->execution;
                bool ready = false;
                bool partial = false;
                StatusCode failure = StatusCode::Ok;
                DataImpact impact = DataImpact::None;
                {
                    std::lock_guard<std::mutex> registered(existing->registration);
                    ready = existing->result_ready && !existing->draining;
                    partial = existing->result_partial;
                    failure = existing->failure;
                    impact = existing->result_impact;
                }
                control.unlock();
                if (ready) {
                    finish_execution(
                        entry, *current->current_operation_id, current->control_generation, failure,
                        partial, impact);
                }
                return;
            }
            if (entry->execution && !entry->quiescent) {
                return;
            }
            execution->purpose = purpose;
            execution->control = current;
            execution->config = std::atomic_load(&entry->published)->config;
            job->owner = shared_from_this();
            job->entry = entry;
            job->execution = execution;
            entry->execution = execution;
            entry->quiescent = false;
            auto publication = task::make_task_publication(
                *entry, execution->config,
                std::atomic_load(&entry->published)->snapshot.snapshot_sequence + 1);
            if (publication.ok()) {
                std::atomic_store(&entry->published, std::move(publication.value()));
            }
        }
        auto queued = options->execution->preparation_lane->submit(job);
        if (!queued.ok()) {
            {
                std::lock_guard<std::mutex> lock(execution->registration);
                execution->preparing = false;
                execution->preparation_returned = true;
                execution->failure = queued.status().code();
                execution->result_ready = true;
                execution->returned.notify_all();
            }
            if (purpose != task::RunPurpose::Operation) {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                if (entry->execution == execution) {
                    entry->quiescent = true;  // No actual work or Native owner was admitted.
                    if (purpose == task::RunPurpose::VolumeRecovery) {
                        std::lock_guard<std::mutex> registered(execution->registration);
                        execution->recovery_exhausted =
                            entry->volume_recovery &&
                            entry->volume_recovery->attempts >=
                                options->execution->runtime_recovery.max_attempts;
                    }
                }
            } else {
                finish_execution(
                    entry, *execution->control->current_operation_id,
                    execution->control->control_generation, queued.status().code());
            }
        }
    } catch (...) {
        // Durable intent remains visible; a later same-key retry may dispatch
        // work that was never created. Never invent a Native completion here.
    }
}

void TaskService::State::finish_execution(
    const std::shared_ptr<task::TaskEntry>& entry, OperationId operation,
    ControlGeneration generation, StatusCode failure, bool partial, DataImpact impact) noexcept {
    try {
        auto job = std::make_shared<ControlJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        job->reply = std::make_shared<task::Reply<OperationId>>();
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            const auto* current = current_operation(*entry->controller->durable_record());
            if (!current || !(current->operation_id == operation) ||
                current->control_generation != generation || task::terminal(current->state)) {
                return;
            }
            auto result = *current;
            result.state = failure != StatusCode::Ok ? OperationState::Failed
                           : partial                 ? OperationState::PartiallySucceeded
                                                     : OperationState::Succeeded;
            result.status_code = failure;
            result.data_impact = impact;
            result.terminal_at = std::max(task::control_wall_now(), result.accepted_at);
            if (failure != StatusCode::Ok) {
                result.failure_domain = FailureDomain::Pipeline;
                result.retryability = Retryability::ManualRecoveryRequired;
            }
            auto work = entry->controller->update_operation(result);
            if (!work.ok()) {
                return;
            }
            job->frame = work.value();
            entry->control_work = work.value();
            entry->operation_reply = job->reply;
            entry->persisting = true;
        }
        auto queued = options->control_lane->submit(job);
        if (!queued.ok()) {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            entry->persisting = false;
            entry->ready_to_submit = true;
        }
    } catch (...) {
        // Current durable Accepted is retained, without a false terminal result.
    }
}

void TaskService::State::PrepareJob::run() noexcept {
    StatusCode failure = StatusCode::Ok;
    bool partial = false;
    std::uint64_t recording_base = 0;
    try {
        auto admitted = owner->validate_deployment(*execution->config);
        if (!admitted.ok()) {
            failure = admitted.status().code();
        }
        auto inputs = failure == StatusCode::Ok
                          ? owner->options->execution->resolve(
                                *execution->control, *execution->config, *entry->identities)
                          : Result<task::RunInputs>(Status(failure, {}));
        auto identities = failure == StatusCode::Ok
                              ? entry->identities->load()
                              : Result<pipeline::StreamIdentityState>(Status(failure, {}));
        if (failure == StatusCode::Ok && identities.ok()) {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            entry->identity = std::move(identities.value());
        } else if (failure == StatusCode::Ok && inputs.ok()) {
            inputs = identities.status();
        }
        if (failure == StatusCode::Ok && !inputs.ok()) {
            failure = inputs.status().code();
        }
        if (failure == StatusCode::Ok) {
            auto& selected = inputs.value();
            auto recovered_time = [&] {
                std::lock_guard<std::mutex> recovery(entry->recovery_execution);
                return entry->recovery->load();
            }();
            if (recovered_time.ok()) {
                if (!recovered_time.value().recording_time_ns) {
                    failure = StatusCode::Unavailable;
                } else {
                    recording_base = *recovered_time.value().recording_time_ns;
                }
            } else if (recovered_time.status().code() != StatusCode::NotFound) {
                failure = recovered_time.status().code();
            }
            if (selected.pipelines.size() != execution->config->pipelines.size()) {
                failure = StatusCode::InvalidArgument;
            }
            for (const auto& pipeline : selected.pipelines) {
                if (!pipeline.controller) {
                    failure = StatusCode::InvalidArgument;
                    break;
                }
                auto state = pipeline.controller->snapshot();
                if (!state.ok()) {
                    failure = state.status().code();
                    break;
                }
                const auto found = std::find_if(
                    execution->config->pipelines.begin(), execution->config->pipelines.end(),
                    [&](const auto& definition) {
                        return definition.pipeline_id == state.value().identity.pipeline_id &&
                               definition.required == pipeline.required;
                    });
                if (found == execution->config->pipelines.end()) {
                    failure = StatusCode::InvalidArgument;
                }
            }
            if (failure == StatusCode::Ok) {
                auto pipelines = selected.pipelines;
                auto boot = selected.boot;
                auto authorization = task::RunAuthorization::create(
                    *execution->control, execution->config->start_policy,
                    std::move(selected.pipelines), entry->recovery, std::move(selected.boot),
                    owner->options->execution->sample);
                if (!authorization.ok()) {
                    failure = authorization.status().code();
                } else {
                    std::shared_ptr<task::RunAuthorization> run(std::move(authorization.value()));
                    {
                        std::lock_guard<std::mutex> lock(execution->registration);
                        execution->pipelines = std::move(pipelines);
                        execution->boot = std::move(boot);
                        execution->sample = owner->options->execution->sample;
                        execution->authorization = run;
                        if (execution->fenced) {
                            (void)run->fence();
                        }
                    }
                    auto deadline =
                        plugin::Deadline::after(owner->options->execution->native_timeout);
                    auto prepared = deadline.ok()
                                        ? run->prepare(deadline.value())
                                        : Result<task::RunPrepareResult>(deadline.status());
                    if (prepared.ok()) {
                        {
                            std::lock_guard<std::mutex> registered(execution->registration);
                            for (std::size_t i = 0; i < execution->pipelines.size(); ++i) {
                                execution->pipelines[i].prepare_status =
                                    prepared.value().pipelines[i];
                                if (prepared.value().pipelines[i] == StatusCode::Ok) {
                                    execution->pipelines[i].effective_config_id =
                                        prepared.value().effective_config_ids[i];
                                }
                            }
                        }
                        std::lock_guard<std::mutex> recovery(entry->recovery_execution);
                        auto checkpoint =
                            execution->purpose != task::RunPurpose::Operation
                                ? compact_ended_authorization(*entry, *execution->control)
                                : Result<void>{};
                        if (checkpoint.ok()) {
                            checkpoint = run->checkpoint_prepared();
                        }
                        if (!checkpoint.ok()) {
                            prepared = checkpoint.status();
                        }
                    }
                    if (!prepared.ok()) {
                        failure = prepared.status().code();
                    } else if (!prepared.value().barrier_passed) {
                        failure = StatusCode::Unavailable;
                    } else {
                        partial = std::any_of(
                            prepared.value().pipelines.begin(), prepared.value().pipelines.end(),
                            [](auto status) { return status != StatusCode::Ok; });
                        Result<void> authorized;
                        {
                            std::lock_guard<std::mutex> recovery(entry->recovery_execution);
                            authorized = run->publish_authorization(recording_base);
                        }
                        if (!authorized.ok()) {
                            failure = authorized.status().code();
                        } else {
                            const auto opening = execution->sample();
                            std::lock_guard<std::mutex> control(entry->control_mutex);
                            const auto current = entry->controller->durable_record();
                            if (!owner->accepting.load(std::memory_order_acquire) ||
                                entry->execution != execution ||
                                !entry->controller->run_is_current(
                                    *execution->control->current_operation_id,
                                    execution->control->control_generation)) {
                                (void)run->fence();
                                failure = StatusCode::Conflict;
                            } else if (!opening.ok()) {
                                failure = opening.status().code();
                            } else {
                                auto opened = run->open(*current, opening.value().monotonic_ns);
                                failure = opened.ok() ? StatusCode::Ok : opened.status().code();
                                if (opened.ok() &&
                                    execution->purpose == task::RunPurpose::VolumeRecovery) {
                                    entry->volume_recovery.reset();
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::bad_alloc&) {
        failure = StatusCode::ResourceExhausted;
    } catch (...) {
        failure = StatusCode::Internal;
    }
    bool exhausted = false;
    if (execution->purpose == task::RunPurpose::VolumeRecovery && failure != StatusCode::Ok) {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        exhausted =
            entry->volume_recovery && entry->volume_recovery->attempts >=
                                          owner->options->execution->runtime_recovery.max_attempts;
    }
    {
        std::lock_guard<std::mutex> lock(execution->registration);
        execution->recovery_exhausted = exhausted;
        execution->failure = failure;
        execution->result_partial = partial;
        execution->preparing = false;
        execution->preparation_returned = true;
        execution->returned.notify_all();
    }
    if (failure != StatusCode::Ok && owner->cleanup_failed_prepare(entry, execution, failure)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(execution->registration);
        execution->result_ready = true;
        execution->result_impact =
            failure == StatusCode::Ok ? DataImpact::None : DataImpact::PreAdmissionOnly;
    }
    if (execution->purpose == task::RunPurpose::Operation) {
        owner->finish_execution(
            entry, *execution->control->current_operation_id,
            execution->control->control_generation, failure, partial,
            failure == StatusCode::Ok ? DataImpact::None : DataImpact::PreAdmissionOnly);
    }
}

}  // namespace rodaq
