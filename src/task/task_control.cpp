#include "task_service_state.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq {

void TaskService::State::ControlJob::run() noexcept {
    bool consumed = false;
    const auto report = [&](Status error) noexcept {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        Status notification(error.code(), {});
        if (!consumed && !entry->pending_result) {
            entry->pending_result.emplace(
                task::ControlWorkResult{frame->work_id, std::move(error), {}});
        }
        entry->persisting = false;
        reply->complete(std::move(notification));
    };
    try {
        for (;;) {
            const bool configuration = std::any_of(
                frame->candidate->operation_history.entries.begin(),
                frame->candidate->operation_history.entries.end(), [&](const auto& stored) {
                    return frame->candidate->current_operation_id &&
                           stored.snapshot.operation_id ==
                               *frame->candidate->current_operation_id &&
                           stored.snapshot.kind == OperationKind::Configure;
                });
            if (!replay) {
                auto actual = task::execute_control_work(
                    *entry->store, *frame, configuration ? entry->identities.get() : nullptr,
                    owner->options->store.config);
                std::lock_guard<std::mutex> lock(entry->control_mutex);
                entry->pending_result.emplace(std::move(actual));
            }
            std::unique_lock<std::mutex> control(entry->control_mutex);
            auto& actual = *entry->pending_result;
            if (configuration && actual.authority.ok() &&
                (!actual.identities || !actual.active_config)) {
                actual.authority =
                    Status(StatusCode::DataLoss, "Incomplete Configure completion facts");
            }
            auto old = std::atomic_load(&entry->published);
            if (old->snapshot.snapshot_sequence == std::numeric_limits<std::uint64_t>::max()) {
                control.unlock();
                report(Status(StatusCode::ResourceExhausted, {}));
                return;
            }
            // Stage both projections before consuming the actual event. Local
            // allocation failure retains that event for replay, never a re-write.
            auto diagnostic = std::make_shared<task::PublishedTask>(*old);
            diagnostic->snapshot.execution_condition = ExecutionCondition::Protected;
            diagnostic->snapshot.snapshot_sequence++;
            diagnostic->snapshot.captured_at = task::control_wall_now();
            std::shared_ptr<const task::PublishedTask> staged;
            if (actual.authority.ok()) {
                auto authority =
                    std::make_shared<const task::TaskControlRecord>(actual.authority.value());
                auto value = task::make_task_publication(
                    *entry, configuration ? actual.active_config : old->config,
                    old->snapshot.snapshot_sequence + 1, std::move(authority));
                if (!value.ok()) {
                    control.unlock();
                    report(value.status());
                    return;
                }
                staged = std::move(value.value());
            }
            auto completed = entry->controller->persistence_completed(
                actual.work_id, actual.authority, actual.publication_failure);
            if (!completed.ok()) {
                control.unlock();
                report(completed.status());
                return;
            }
            if (!completed.value().recognized || !completed.value().resolved) {
                control.unlock();
                report(Status(StatusCode::DataLoss, "Unrecognized Configure work result"));
                return;
            }
            consumed = true;
            if (completed.value().persistence_protected) {
                std::atomic_store(
                    &entry->published,
                    std::shared_ptr<const task::PublishedTask>(std::move(diagnostic)));
                entry->pending_result.reset();
                entry->persisting = false;
                reply->complete(std::move(completed.value().resolved->status));
                if (completed.value().stop_resolution && entry->stop_reply) {
                    entry->stop_reply->complete(completed.value().stop_resolution->status);
                }
                return;
            }
            if (configuration) {
                entry->identity = std::move(*actual.identities);
            }
            std::atomic_store(&entry->published, std::move(staged));
            entry->pending_result.reset();
            entry->control_work.reset();
            entry->persisting = false;
            auto& resolution = *completed.value().resolved;
            if (completed.value().stop_resolution && entry->stop_reply) {
                entry->stop_reply->complete(completed.value().stop_resolution->status);
                entry->stop_reply.reset();
                entry->stop_id.reset();
            }
            if (completed.value().next_work) {
                reply->complete(
                    resolution.durable ? Result<OperationId>(resolution.operation_id)
                                       : Result<OperationId>(resolution.status));
                frame = std::move(completed.value().next_work);
                entry->control_work = frame;
                reply = entry->stop_reply;
                entry->operation_reply = reply;
                entry->stop_reply.reset();
                entry->stop_id.reset();
                entry->persisting = true;
                consumed = false;
                replay = false;
                control.unlock();
                continue;
            }
            if (!resolution.durable) {
                if (frame->publication == task::ControlPublicationKind::OperationUpdate) {
                    // The configuration remains Accepted, but its terminal
                    // fact still needs recovery. Do not advertise healthy Idle.
                    std::atomic_store(
                        &entry->published,
                        std::shared_ptr<const task::PublishedTask>(std::move(diagnostic)));
                }
                reply->complete(std::move(resolution.status));
                return;
            }
            // Accepted is independent of the subsequent terminal-status write.
            // The same fixed actual job may then publish that result, without
            // allocating a second executor task or consuming another queue slot.
            auto operation = std::atomic_load(&entry->published)->snapshot.current_operation;
            if (!configuration && frame->publication == task::ControlPublicationKind::Intent &&
                operation &&
                (operation->kind == OperationKind::Pause ||
                 operation->kind == OperationKind::Stop)) {
                ++entry->closing;
                auto accepted = entry->controller->durable_record();
                control.unlock();
                owner->close_execution(entry, reply, std::move(accepted));
                return;
            }
            reply->complete(resolution.operation_id);
            if (frame->publication == task::ControlPublicationKind::OperationUpdate) {
                return;
            }
            auto terminal = task::completed_configuration(*entry->controller->durable_record());
            if (!terminal) {
                control.unlock();
                if (operation && (operation->kind == OperationKind::Start ||
                                  operation->kind == OperationKind::Resume)) {
                    owner->start_execution(entry);
                } else if (operation && operation->kind == OperationKind::Reset) {
                    owner->start_reset(entry);
                } else if (operation && operation->kind == OperationKind::Retire) {
                    owner->start_retire(entry);
                }
                return;
            }
            auto status = entry->controller->update_operation(*terminal);
            if (!status.ok()) {
                return;
            }
            frame = std::move(status.value());
            entry->control_work = frame;
            entry->persisting = true;
            consumed = false;
            replay = false;
            control.unlock();
        }
    } catch (const std::bad_alloc&) {
        report(Status(StatusCode::ResourceExhausted, {}));
    } catch (...) {
        report(Status(StatusCode::Internal, {}));
    }
}

}  // namespace rodaq
