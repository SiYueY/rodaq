#include "task_service_state.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq::task {

void RunExecution::fence() noexcept {
    std::lock_guard<std::mutex> lock(registration);
    fenced = true;
    if (authorization) {
        (void)authorization->fence();
    }
}

ExecutionCondition RunExecution::condition(DesiredState desired) {
    std::lock_guard<std::mutex> lock(registration);
    if (draining) {
        return ExecutionCondition::Draining;
    }
    if (preparing) {
        return ExecutionCondition::Preparing;
    }
    bool all_settled = preparation_returned && !rotating;
    bool any_open = false;
    bool all_open = !pipelines.empty();
    bool required_open = true;
    for (const auto& selected : pipelines) {
        auto actual = selected.controller->snapshot();
        if (!actual.ok()) {
            return ExecutionCondition::Protected;
        }
        all_settled = all_settled && actual.value().quiescence.settled;
        const bool open = actual.value().gate.open && !actual.value().gate.control_fence &&
                          actual.value().failure == StatusCode::Ok;
        any_open = any_open || open;
        all_open = all_open && open;
        required_open = required_open && (!selected.required || open);
    }
    if (any_open) {
        if (!required_open && config->start_policy == StartPolicyMode::AllRequired) {
            return ExecutionCondition::Protected;
        }
        return all_open ? ExecutionCondition::Active : ExecutionCondition::Degraded;
    }
    return all_settled ? (desired == DesiredState::Running ? ExecutionCondition::Protected
                                                           : ExecutionCondition::Idle)
                       : ExecutionCondition::PendingCleanup;
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

}  // namespace

void TaskService::State::start_execution(const std::shared_ptr<task::TaskEntry>& entry) noexcept {
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
                !options->execution || task::terminal(operation->state)) {
                return;
            }
            if (std::atomic_load(&entry->published)->snapshot.snapshot_sequence ==
                std::numeric_limits<std::uint64_t>::max()) {
                return;
            }
            if (entry->execution &&
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
            finish_execution(
                entry, *execution->control->current_operation_id,
                execution->control->control_generation, queued.status().code());
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
    try {
        auto inputs = owner->options->execution->resolve(
            *execution->control, *execution->config, *entry->identities);
        auto identities = entry->identities->load();
        if (identities.ok()) {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            entry->identity = std::move(identities.value());
        } else if (inputs.ok()) {
            inputs = identities.status();
        }
        if (!inputs.ok()) {
            failure = inputs.status().code();
        } else {
            auto& selected = inputs.value();
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
                    selected.sample);
                if (!authorization.ok()) {
                    failure = authorization.status().code();
                } else {
                    std::shared_ptr<task::RunAuthorization> run(std::move(authorization.value()));
                    {
                        std::lock_guard<std::mutex> lock(execution->registration);
                        execution->pipelines = std::move(pipelines);
                        execution->boot = std::move(boot);
                        execution->sample = selected.sample;
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
                        auto checkpoint = run->checkpoint_prepared();
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
                            authorized = run->publish_authorization(selected.recording_base);
                        }
                        if (!authorized.ok()) {
                            failure = authorized.status().code();
                        } else {
                            std::lock_guard<std::mutex> control(entry->control_mutex);
                            const auto current = entry->controller->durable_record();
                            if (!owner->accepting.load(std::memory_order_acquire) ||
                                !entry->controller->run_is_current(
                                    *execution->control->current_operation_id,
                                    execution->control->control_generation)) {
                                (void)run->fence();
                                failure = StatusCode::Conflict;
                            } else {
                                auto opened = run->open(*current);
                                failure = opened.ok() ? StatusCode::Ok : opened.status().code();
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
    {
        std::lock_guard<std::mutex> lock(execution->registration);
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
    owner->finish_execution(
        entry, *execution->control->current_operation_id, execution->control->control_generation,
        failure, partial,
        failure == StatusCode::Ok ? DataImpact::None : DataImpact::PreAdmissionOnly);
}

}  // namespace rodaq
