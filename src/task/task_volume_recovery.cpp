#include "task_service_state.hpp"

#include <chrono>
#include <mutex>
#include <new>
#include <vector>

#include "../artifact/volume_capacity.hpp"

namespace rodaq::task {

bool valid_runtime_recovery_policy(const RuntimeRecoveryPolicy& policy) noexcept {
    return policy.max_attempts != 0 && policy.max_attempts <= 1024 &&
           policy.initial_backoff > std::chrono::nanoseconds::zero() &&
           policy.max_backoff >= policy.initial_backoff &&
           policy.max_backoff <= std::chrono::hours{24};
}

}  // namespace rodaq::task

namespace rodaq {

Result<void> TaskService::reconcile_volume(
    artifact::VolumeSpaceCondition condition, std::uint64_t now_monotonic) {
    if (!state_->options->execution || !state_->accepting.load(std::memory_order_acquire)) {
        return {};
    }
    try {
        std::vector<std::shared_ptr<task::TaskEntry>> entries;
        {
            std::lock_guard<std::mutex> catalog(state_->catalog_mutex);
            entries = state_->entries;
        }
        for (const auto& entry : entries) {
            bool recover = false;
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                const auto current = entry->controller->durable_record();
                if (entry->volume_recovery &&
                    (entry->volume_recovery->generation != current->control_generation ||
                     current->desired_state != DesiredState::Running)) {
                    entry->volume_recovery.reset();
                }
                if (condition != artifact::VolumeSpaceCondition::Normal &&
                    condition != artifact::VolumeSpaceCondition::Low &&
                    current->desired_state == DesiredState::Running && !entry->volume_recovery &&
                    entry->execution) {
                    std::lock_guard<std::mutex> registered(entry->execution->registration);
                    for (const auto& pipeline : entry->execution->pipelines) {
                        auto facts = pipeline.controller->snapshot();
                        if (facts.ok() && facts.value().gate.ever_opened) {
                            entry->volume_recovery =
                                task::VolumeRecovery{current->control_generation, 0, now_monotonic};
                            break;
                        }
                    }
                }
                const auto config = std::atomic_load(&entry->published)->config;
                recover = condition == artifact::VolumeSpaceCondition::Normal &&
                          entry->volume_recovery &&
                          entry->volume_recovery->attempts <
                              state_->options->execution->runtime_recovery.max_attempts &&
                          now_monotonic >= entry->volume_recovery->not_before && entry->quiescent &&
                          !entry->persisting && !entry->control_work && !entry->closing &&
                          entry->cleanup_active_id == 0 && !entry->cleanup_ready.frame &&
                          !entry->cleanup_pending.frame && current->current_operation_id &&
                          entry->controller->run_is_current(
                              *current->current_operation_id, current->control_generation) &&
                          config->recovery_policy == RecoveryPolicyMode::Resume;
            }
            if (recover) {
                state_->start_execution(entry, task::RunPurpose::VolumeRecovery, now_monotonic);
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
