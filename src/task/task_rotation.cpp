#include "task_service_state.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace rodaq {

Result<void> TaskService::schedule_rotation(std::uint64_t now_monotonic) {
    if (!state_->accepting.load(std::memory_order_acquire) || !state_->options->execution) {
        return {};
    }
    try {
        std::vector<std::shared_ptr<task::TaskEntry>> entries;
        {
            std::lock_guard<std::mutex> catalog(state_->catalog_mutex);
            entries = state_->entries;
        }
        for (const auto& entry : entries) {
            auto job = std::make_shared<State::RotateJob>();
            job->owner = state_;
            job->entry = entry;
            job->now_monotonic = now_monotonic;
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                auto execution = entry->execution;
                if (!state_->accepting.load(std::memory_order_acquire) || entry->persisting ||
                    entry->closing || !execution || !execution->control->current_operation_id ||
                    !entry->controller->run_is_current(
                        *execution->control->current_operation_id,
                        execution->control->control_generation)) {
                    continue;
                }
                std::lock_guard<std::mutex> registered(execution->registration);
                if (execution->preparing || execution->draining || execution->fenced ||
                    execution->rotating) {
                    continue;
                }
                bool needed = false;
                for (const auto& pipeline : execution->pipelines) {
                    auto due = pipeline.controller->rotation_due(
                        execution->config->segment_policy, now_monotonic);
                    if (!due.ok()) {
                        return due.status();
                    }
                    needed = needed || due.value();
                }
                if (!needed) {
                    continue;
                }
                job->execution = execution;
                execution->rotating = true;
            }
            auto queued = state_->options->execution->preparation_lane->submit(job);
            if (!queued.ok()) {
                std::lock_guard<std::mutex> registered(job->execution->registration);
                job->execution->rotating = false;
                return queued.status();
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

void TaskService::State::RotateJob::run() noexcept {
    StatusCode failure = StatusCode::Ok;
    try {
        for (const auto& pipeline : execution->pipelines) {
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                if (!owner->accepting.load(std::memory_order_acquire) ||
                    entry->execution != execution ||
                    !entry->controller->run_is_current(
                        *execution->control->current_operation_id,
                        execution->control->control_generation)) {
                    break;
                }
            }
            auto due =
                pipeline.controller->rotation_due(execution->config->segment_policy, now_monotonic);
            if (!due.ok()) {
                failure = due.status().code();
                (void)pipeline.controller->establish_control_fence();
                break;
            }
            if (!due.value()) {
                continue;
            }
            auto snapshot = pipeline.controller->snapshot();
            if (!snapshot.ok()) {
                failure = snapshot.status().code();
                break;
            }
            auto next = snapshot.value().successor_segment_id;
            if (next == 0) {
                auto reserved = owner->options->execution->reserve_segment();
                if (!reserved.ok()) {
                    failure = reserved.status().code();
                    (void)pipeline.controller->establish_control_fence();
                    break;
                }
                next = reserved.value();
            }
            auto deadline = plugin::Deadline::after(owner->options->execution->native_timeout);
            if (!deadline.ok()) {
                failure = deadline.status().code();
                break;
            }
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                std::lock_guard<std::mutex> registered(execution->registration);
                if (!owner->accepting.load(std::memory_order_acquire) || execution->fenced ||
                    execution->draining || entry->execution != execution ||
                    !entry->controller->run_is_current(
                        *execution->control->current_operation_id,
                        execution->control->control_generation)) {
                    break;
                }
                execution->rotating_native = true;
            }
            auto rotated = pipeline.controller->rotate(next, deadline.value());
            {
                std::lock_guard<std::mutex> registered(execution->registration);
                execution->rotating_native = false;
                execution->returned.notify_all();
            }
            if (!rotated.ok()) {
                // A Descriptor cut may have won before this rotation's Gate
                // cut. It burns only the reserved ID and may retry next pass.
                failure = rotated.status().code();
                if (failure != StatusCode::Conflict && failure != StatusCode::Unavailable) {
                    (void)pipeline.controller->establish_control_fence();
                }
                break;
            }
            if (rotated.value()) {
                // Actual accepted work must settle even if Stop superseded
                // this Run during Native Seal. Artifact work never opens Gate.
                const auto work_id = rotated.value()->work_id;
                const auto segment_id = rotated.value()->segment->metadata.segment_id();
                auto controller = pipeline.controller;
                auto committed =
                    owner->options->execution->commit_with_ack
                        ? owner->options->execution->commit_with_ack(
                              *rotated.value(),
                              [controller, work_id, segment_id] {
                                  (void)controller->acknowledge_artifact(work_id, segment_id);
                              })
                        : owner->options->execution->commit(*rotated.value());
                if (committed.ok()) {
                    committed = pipeline.controller->acknowledge_artifact(work_id, segment_id);
                }
                if (!committed.ok()) {
                    failure = committed.status().code();
                    (void)pipeline.controller->establish_control_fence();
                    break;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        failure = StatusCode::ResourceExhausted;
    } catch (...) {
        failure = StatusCode::Internal;
    }
    {
        std::lock_guard<std::mutex> registered(execution->registration);
        execution->rotating = false;
        execution->rotating_native = false;
        if (failure != StatusCode::Ok) {
            execution->failure = failure;
        }
        execution->returned.notify_all();
    }
    // Published runtime facts are refreshed by the owning maintenance pass;
    // this completion does not mutate an already terminal user Operation.
}

}  // namespace rodaq
