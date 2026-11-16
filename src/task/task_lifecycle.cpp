#include "task_service_state.hpp"

#include <new>
#include <utility>

namespace rodaq {

Result<OperationId> TaskService::start(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(id, OperationKind::Start, request, precondition);
}

Result<OperationId> TaskService::pause(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(id, OperationKind::Pause, request, precondition);
}

Result<OperationId> TaskService::resume(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(id, OperationKind::Resume, request, precondition);
}

Result<OperationId> TaskService::stop(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(id, OperationKind::Stop, request, precondition);
}

Result<OperationId> TaskService::lifecycle(
    TaskId id, OperationKind kind, const RequestIdentity& request,
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
        auto entry = state_->find(id);
        if (!entry) {
            return Status(StatusCode::NotFound, {});
        }
        auto reply = std::make_shared<task::Reply<OperationId>>();
        auto job = std::make_shared<State::ControlJob>();
        job->owner = state_;
        job->entry = entry;
        bool dispatch = false;
        bool close = false;
        std::shared_ptr<const task::TaskControlRecord> close_control;
        std::optional<OperationId> accepted;
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            const auto durable = entry->controller->durable_record();
            auto decision = entry->controller->request_lifecycle(
                kind, request, precondition, task::control_wall_now(),
                {entry->quiescent && entry->closing == 0,
                 static_cast<bool>(state_->options->execution)});
            if (!decision.ok()) {
                return decision.status();
            }
            if (decision.value().fence_required && entry->execution) {
                entry->execution->fence();
            }
            if (decision.value().durably_accepted) {
                accepted = decision.value().operation_id;
                const auto published = std::atomic_load(&entry->published);
                const auto& operation = published->snapshot.current_operation;
                if ((kind == OperationKind::Pause || kind == OperationKind::Stop) && operation &&
                    operation->operation_id == *accepted && !task::terminal(operation->state) &&
                    !(entry->closed_acceptance == accepted)) {
                    if (!state_->options->execution) {
                        return Status(StatusCode::InvalidState, {});
                    }
                    accepted.reset();
                    if (entry->closing) {
                        reply = entry->operation_reply;
                    } else {
                        entry->operation_reply = reply;
                        ++entry->closing;
                        close_control = durable;
                        close = true;
                    }
                }
            }
            if (!accepted && !close && kind == OperationKind::Stop && !decision.value().work &&
                entry->control_work &&
                !(entry->control_work->candidate->current_operation_id ==
                  std::optional<OperationId>(decision.value().operation_id))) {
                if (!entry->persisting && (entry->pending_result || entry->ready_to_submit ||
                                           entry->controller->persistence_protected())) {
                    job->replay = entry->pending_result.has_value();
                    if (!job->replay && !entry->ready_to_submit) {
                        auto reconcile = entry->controller->reconciliation_work();
                        if (!reconcile.ok()) {
                            return reconcile.status();
                        }
                        entry->control_work = std::move(reconcile.value());
                    }
                    entry->stop_id = decision.value().operation_id;
                    entry->stop_reply = reply;
                    job->reply = entry->operation_reply;
                    job->frame = entry->control_work;
                    entry->persisting = true;
                    entry->ready_to_submit = false;
                    dispatch = true;
                } else if (!entry->stop_id) {
                    entry->stop_id = decision.value().operation_id;
                    entry->stop_reply = reply;
                } else {
                    reply = entry->stop_reply;
                }
            } else if (!close) {
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
                        auto reconciled = entry->controller->reconciliation_work();
                        if (!reconciled.ok()) {
                            return accepted ? Result<OperationId>(*accepted)
                                            : Result<OperationId>(reconciled.status());
                        }
                        entry->control_work = std::move(reconciled.value());
                        dispatch = true;
                    }
                }
                if (dispatch) {
                    entry->operation_reply = reply;
                    entry->persisting = true;
                    entry->ready_to_submit = false;
                    job->frame = entry->control_work;
                    job->reply = reply;
                } else if (!accepted) {
                    reply = entry->operation_reply;
                }
            }
        }
        if (close) {
            state_->close_execution(entry, reply, std::move(close_control));
        } else if (dispatch) {
            auto queued = state_->options->control_lane->submit(job);
            if (!queued.ok()) {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                entry->persisting = false;
                entry->ready_to_submit = !job->replay;
                reply->complete(queued.status());
            }
        }
        if (accepted) {
            if (kind == OperationKind::Start || kind == OperationKind::Resume) {
                state_->start_execution(entry);
            }
            return *accepted;
        }
        return reply ? reply->wait(state_->options->request_wait)
                     : Result<OperationId>(Status(StatusCode::Internal, {}));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
