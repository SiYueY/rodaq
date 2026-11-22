#include "task_service_state.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace rodaq {

Result<void> TaskService::close_admission() {
    state_->accepting.store(false, std::memory_order_release);
    // Registry then Task is the same order as registry publication. No I/O,
    // Native operation or wait is performed under either structural lock.
    std::lock_guard<std::mutex> catalog(state_->catalog_mutex);
    for (const auto& entry : state_->entries) {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        if (entry->execution) {
            entry->execution->fence();
        }
    }
    return {};
}

Result<void> TaskService::drain_shutdown(bool pending_only) {
    try {
        std::vector<std::shared_ptr<task::TaskEntry>> entries;
        {
            std::lock_guard<std::mutex> catalog(state_->catalog_mutex);
            entries = state_->entries;
        }
        for (const auto& entry : entries) {
            if (pending_only) {
                state_->cleanup_failed_pipelines(entry);
            }
            auto retried = state_->retry_cleanup(entry);
            if (!retried.ok()) {
                return retried.status();
            }
            auto job = std::make_shared<State::CloseJob>();
            job->owner = state_;
            job->entry = entry;
            job->reply = std::make_shared<task::Reply<OperationId>>();
            job->process_drain = true;
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                if (entry->quiescent || entry->closing) {
                    continue;
                }
                if (pending_only) {
                    if (entry->persisting || entry->controller->persistence_protected() ||
                        !entry->execution ||
                        entry->execution->condition(
                            entry->controller->durable_record()->desired_state) !=
                            ExecutionCondition::PendingCleanup) {
                        continue;
                    }
                    bool ready = true;
                    std::lock_guard<std::mutex> registered(entry->execution->registration);
                    for (const auto& selected : entry->execution->pipelines) {
                        auto facts = selected.controller->snapshot();
                        if (!facts.ok() || !facts.value().quiescence.control_fenced ||
                            facts.value().quiescence.control_inflight != 0 ||
                            facts.value().quiescence.writer_inflight != 0 ||
                            facts.value().quiescence.callbacks != 0 ||
                            facts.value().quiescence.commit_inflight != 0 ||
                            facts.value().quiescence.admission_candidates != 0) {
                            ready = false;
                            break;
                        }
                    }
                    if (!ready) {
                        continue;
                    }
                }
                if (!entry->execution || !state_->options->execution) {
                    // A protected recovered Task is not proof of quiescence.
                    return Status(StatusCode::Unavailable, {});
                }
                job->control = entry->controller->durable_record();
                job->execution = entry->execution;
                job->execution->fence();
                std::lock_guard<std::mutex> registered(job->execution->registration);
                job->execution->draining = true;
                ++entry->closing;
            }
            auto queued = state_->enqueue_cleanup(entry, job);
            if (!queued.ok()) {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                --entry->closing;
                std::lock_guard<std::mutex> registered(job->execution->registration);
                job->execution->draining = false;
                return queued.status();
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> TaskService::refresh_execution_facts(bool protect) {
    try {
        std::vector<std::shared_ptr<task::TaskEntry>> entries;
        {
            std::lock_guard<std::mutex> catalog(state_->catalog_mutex);
            entries = state_->entries;
        }
        for (const auto& entry : entries) {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            if (protect && entry->execution) {
                entry->execution->fence();
            }
            auto old = std::atomic_load(&entry->published);
            if (old->snapshot.snapshot_sequence == std::numeric_limits<std::uint64_t>::max()) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            auto fresh = task::make_task_publication(
                *entry, old->config, old->snapshot.snapshot_sequence + 1);
            if (!fresh.ok()) {
                return fresh.status();
            }
            std::atomic_store(&entry->published, std::move(fresh.value()));
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

bool TaskService::shutdown_settled() const {
    std::lock_guard<std::mutex> catalog(state_->catalog_mutex);
    for (const auto& entry : state_->entries) {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        if (!entry->quiescent || entry->closing || entry->persisting ||
            entry->cleanup_active_id != 0 || entry->cleanup_ready.frame ||
            entry->cleanup_pending.frame) {
            return false;
        }
    }
    return true;
}

}  // namespace rodaq
