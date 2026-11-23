#include "task_service_state.hpp"

#include "config.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq {
namespace task {

std::optional<OperationSnapshot> completed_configuration(const TaskControlRecord& control) {
    if (!control.current_operation_id) {
        return std::nullopt;
    }
    for (const auto& operation : control.operation_history.entries) {
        if (operation.snapshot.operation_id == *control.current_operation_id &&
            operation.snapshot.kind == OperationKind::Configure &&
            !terminal(operation.snapshot.state)) {
            auto result = operation.snapshot;
            result.state = OperationState::Succeeded;
            result.terminal_at = std::max(control_wall_now(), result.accepted_at);
            return result;
        }
    }
    return std::nullopt;
}

}  // namespace task

Result<OperationId> TaskService::configure(
    TaskId id, const TaskConfig& config, const RequestIdentity& request,
    const ControlPrecondition& precondition) {
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, {});
    }
    auto count = state_->requests.load(std::memory_order_relaxed);
    do {
        if (count == state_->options->max_requests) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    } while (!state_->requests.compare_exchange_weak(count, count + 1, std::memory_order_acquire));
    task::RequestSlot slot(state_->requests);
    try {
        auto normalized = task::normalize_config(config, state_->options->store.config);
        if (!normalized.ok()) {
            return normalized.status();
        }
        auto entry = state_->find(id);
        if (!entry) {
            return Status(StatusCode::NotFound, {});
        }
        auto reply = std::make_shared<task::Reply<OperationId>>();
        auto job = std::make_shared<State::ControlJob>();
        job->owner = state_;
        job->entry = entry;
        std::optional<OperationId> already_accepted;
        bool dispatch = false;
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            if (!state_->accepting.load(std::memory_order_acquire)) {
                return Status(StatusCode::InvalidState, {});
            }
            auto current = std::atomic_load(&entry->published);
            const auto allowed = state_->validate_deployment(normalized.value());
            task::ConfigureFacts facts{
                *current->config, entry->identity,
                entry->quiescent && !entry->retention_work &&
                    current->snapshot.snapshot_sequence !=
                        std::numeric_limits<std::uint64_t>::max(),
                allowed.ok()};
            auto decision = entry->controller->request_configure(
                normalized.value(), request, precondition, task::control_wall_now(), facts,
                state_->options->store.config);
            if (!decision.ok()) {
                return decision.status();
            }
            if (decision.value().durably_accepted) {
                already_accepted = decision.value().operation_id;
            }
            if (decision.value().work) {
                entry->control_work = decision.value().work;
                entry->ready_to_submit = true;
            }
            if (!entry->persisting) {
                if (entry->pending_result) {
                    job->replay = true;
                    dispatch = true;
                } else if (entry->ready_to_submit) {
                    dispatch = true;
                } else if (entry->controller->persistence_protected()) {
                    auto reconcile = entry->controller->reconciliation_work();
                    if (!reconcile.ok()) {
                        return already_accepted ? Result<OperationId>(*already_accepted)
                                                : Result<OperationId>(reconcile.status());
                    }
                    entry->control_work = std::move(reconcile.value());
                    dispatch = true;
                } else if (already_accepted && !entry->control_work) {
                    auto terminal =
                        task::completed_configuration(*entry->controller->durable_record());
                    if (terminal) {
                        auto work = entry->controller->update_operation(*terminal);
                        if (work.ok()) {
                            entry->control_work = std::move(work.value());
                            dispatch = true;
                        }
                    }
                }
            }
            if (dispatch) {
                entry->operation_reply = reply;
                entry->persisting = true;
                entry->ready_to_submit = false;
                job->reply = reply;
                job->frame = entry->control_work;
            } else if (!already_accepted) {
                if (!entry->operation_reply) {
                    return Status(StatusCode::Internal, {});
                }
                reply = entry->operation_reply;
            }
        }
        if (dispatch) {
            auto submitted = state_->options->control_lane->submit(job);
            if (!submitted.ok()) {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                entry->persisting = false;
                entry->ready_to_submit =
                    !job->replay && job->frame->kind == task::ControlWorkKind::Publish;
                if (!entry->ready_to_submit && !entry->pending_result) {
                    entry->pending_result.emplace(
                        task::ControlWorkResult{job->frame->work_id, submitted.status(), {}});
                }
                reply->complete(std::move(submitted).status());
            }
        }
        if (already_accepted) {
            return *already_accepted;
        }
        return reply->wait(state_->options->request_wait);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
