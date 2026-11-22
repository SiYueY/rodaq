#include "task_service_state.hpp"

#include <memory>
#include <mutex>

namespace rodaq {

void TaskService::State::cleanup_failed_pipelines(
    const std::shared_ptr<task::TaskEntry>& entry) noexcept {
    try {
        std::shared_ptr<task::RunExecution> selected;
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            const auto execution = entry->execution;
            if (!accepting.load(std::memory_order_acquire) || !options->execution ||
                entry->closing || !execution || !execution->control->current_operation_id ||
                !entry->controller->run_is_current(
                    *execution->control->current_operation_id,
                    execution->control->control_generation)) {
                return;
            }
            std::lock_guard<std::mutex> registered(execution->registration);
            if (!execution->preparation_returned || execution->preparing || execution->draining ||
                execution->fenced || !execution->result_partial ||
                execution->failure != StatusCode::Ok) {
                return;
            }
            bool needed = false;
            for (const auto& pipeline : execution->pipelines) {
                if (pipeline.prepare_status && *pipeline.prepare_status != StatusCode::Ok) {
                    const auto facts = pipeline.controller->snapshot();
                    needed = needed || (facts.ok() && !facts.value().quiescence.settled);
                }
            }
            if (!needed) {
                return;
            }
            selected = execution;
        }
        auto job = std::make_shared<FailedPipelineCleanup>();
        job->owner = shared_from_this();
        job->execution = std::move(selected);
        // Maintenance never consumes the deferred slot reserved for a user
        // lifecycle operation. An occupied lane is retried by the next observe.
        (void)enqueue_cleanup(entry, std::move(job), false);
    } catch (...) {
        // No user operation is created or completed by this best-effort
        // dispatch. Retained Pipeline facts remain unsettled for the next pass.
    }
}

void TaskService::State::FailedPipelineCleanup::run() noexcept {
    try {
        for (const auto& pipeline : execution->pipelines) {
            if (!pipeline.prepare_status || *pipeline.prepare_status == StatusCode::Ok) {
                continue;
            }
            // Failed Prepare never crossed Open: there is no accepted set or sealed
            // handoff to consume. Do not drain successful peers or alter Run intent.
            auto facts = pipeline.controller->snapshot();
            if (!facts.ok() || facts.value().quiescence.settled ||
                facts.value().records.accepted != 0) {
                continue;
            }
            auto fenced =
                pipeline.controller->close_admission(execution->control->control_generation);
            auto deadline = plugin::Deadline::after(owner->options->execution->native_timeout);
            if (fenced.ok() && deadline.ok()) {
                (void)pipeline.controller->cleanup(deadline.value());
            }
        }
    } catch (...) {
        // The next observation retries actual unsettled owners. Cleanup failure
        // never fabricates a user-operation result or releases Native backing.
    }
}

}  // namespace rodaq
