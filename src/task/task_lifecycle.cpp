#include "task_service_state.hpp"

#include "identity.hpp"

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

Result<OperationId> TaskService::reset(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    return lifecycle(id, OperationKind::Reset, request, precondition);
}

Result<OperationId> TaskService::retire(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, {});
    }
    auto valid_req = task::validate_request_identity(request);
    if (!valid_req.ok()) {
        return valid_req.status();
    }
    auto digest = task::lifecycle_request_digest(id, OperationKind::Retire, precondition);
    if (!digest.ok()) {
        return digest.status();
    }
    // Check durable retirement tombstone authority first so idempotent Retire
    // retries recover the original operation_id/Succeeded even after the active
    // Task namespace has been cleaned up, and conflicting digests return Conflict.
    auto retired = state_->retired->contains(id);
    if (!retired.ok()) {
        return retired.status();
    }
    if (retired.value()) {
        return state_->retired->lookup_request(id, request, digest.value());
    }
    // For a fresh Retire on an active Task, verify artifact-side eligibility
    // through the sole Artifact & Volume predicate before accepting if this is
    // not already a durable/pending idempotent retry.
    auto entry = state_->find(id);
    if (!entry) {
        return Status(StatusCode::NotFound, {});
    }
    bool already_known = false;
    {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        const auto durable = entry->controller->durable_record();
        auto known = task::find_request(durable->operation_history, request, digest.value());
        if (known.ok()) {
            already_known = true;
        } else if (known.status().code() != StatusCode::NotFound) {
            return known.status();
        }
        if (!already_known && entry->control_work &&
            entry->control_work->publication == task::ControlPublicationKind::Intent &&
            entry->control_work->candidate->current_operation_id) {
            for (const auto& op : entry->control_work->candidate->operation_history.entries) {
                if (op.snapshot.operation_id ==
                        *entry->control_work->candidate->current_operation_id &&
                    op.request.request_key == request.request_key) {
                    already_known = true;
                    break;
                }
            }
        }
        if (!already_known) {
            if (precondition.has_expected_control_generation &&
                precondition.expected_control_generation != durable->control_generation) {
                return Status(StatusCode::Conflict, {});
            }
            const bool settled_quiescent =
                entry->quiescent && entry->closing == 0 && !entry->retention_work &&
                !entry->artifact_maintenance_active && !entry->resetting && !entry->retiring &&
                entry->cleanup_active_id == 0 && !entry->cleanup_ready.frame &&
                !entry->cleanup_pending.frame;
            bool non_terminal_op = false;
            if (durable->current_operation_id) {
                for (const auto& op : durable->operation_history.entries) {
                    if (op.snapshot.operation_id == *durable->current_operation_id &&
                        !task::terminal(op.snapshot.state)) {
                        non_terminal_op = true;
                        break;
                    }
                }
            }
            if (!state_->options->execution || durable->desired_state != DesiredState::Stopped ||
                durable->reset_intent || non_terminal_op || !settled_quiescent ||
                entry->controller->persistence_protected()) {
                return Status(StatusCode::InvalidState, {});
            }
            if (entry->persisting || entry->control_work) {
                return Status(StatusCode::Conflict, {});
            }
        }
    }
    if (!already_known && state_->options->execution &&
        state_->options->execution->artifacts.retire_eligible) {
        auto eligible = state_->options->execution->artifacts.retire_eligible(id);
        if (!eligible.ok()) {
            return eligible.status();
        }
        if (!eligible.value()) {
            return Status(StatusCode::Conflict, {});
        }
    }
    return lifecycle(id, OperationKind::Retire, request, precondition);
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
        std::optional<std::uint64_t> cut;
        if ((kind == OperationKind::Pause || kind == OperationKind::Stop) &&
            state_->options->execution) {
            const auto sample = state_->options->execution->sample();
            if (sample.ok()) {
                cut = sample.value().monotonic_ns;
            }
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
            if (!state_->accepting.load(std::memory_order_acquire)) {
                return Status(StatusCode::InvalidState, {});
            }
            const auto durable = entry->controller->durable_record();
            const bool settled_quiescent =
                entry->quiescent && entry->closing == 0 && !entry->retention_work &&
                !entry->artifact_maintenance_active && !entry->resetting && !entry->retiring &&
                entry->cleanup_active_id == 0 && !entry->cleanup_ready.frame &&
                !entry->cleanup_pending.frame;
            auto decision = entry->controller->request_lifecycle(
                kind, request, precondition, task::control_wall_now(),
                {settled_quiescent, static_cast<bool>(state_->options->execution)});
            if (!decision.ok()) {
                return decision.status();
            }
            if (decision.value().fence_required && entry->execution) {
                entry->execution->fence(cut);
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
            } else if (kind == OperationKind::Reset) {
                state_->start_reset(entry);
            } else if (kind == OperationKind::Retire) {
                state_->start_retire(entry);
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
