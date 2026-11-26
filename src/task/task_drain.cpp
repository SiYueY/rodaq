#include "task_service_state.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace rodaq {
namespace {

struct CloseFlight {
    task::TaskEntry& entry;
    std::shared_ptr<task::RunExecution> execution;
    bool active{true};

    void finish() noexcept {
        if (!active) {
            return;
        }
        std::lock_guard<std::mutex> control(entry.control_mutex);
        --entry.closing;
        if (execution) {
            std::lock_guard<std::mutex> registered(execution->registration);
            execution->draining = entry.closing != 0;
        }
        active = false;
    }

    ~CloseFlight() noexcept { finish(); }
};

Result<void> revoke_continuation(task::TaskEntry& entry, const task::TaskControlRecord& control) {
    std::lock_guard<std::mutex> execution(entry.recovery_execution);
    auto old = entry.recovery->load();
    if (!old.ok() && old.status().code() != StatusCode::NotFound) {
        auto reconciled = entry.recovery->reconcile();
        if (!reconciled.ok()) {
            return reconciled.status();
        }
        old = reconciled.value()
                  ? Result<task::TaskRecoveryRecord>(*reconciled.value())
                  : Result<task::TaskRecoveryRecord>(Status(StatusCode::NotFound, {}));
    }
    task::TaskRecoveryRecord record;
    std::uint64_t expected = 0;
    if (old.ok()) {
        record = std::move(old.value());
        if (record.observed_control_generation > control.control_generation ||
            record.record_revision == std::numeric_limits<std::uint64_t>::max()) {
            return Status(StatusCode::Conflict, {});
        }
        expected = record.record_revision++;
    } else {
        // No prior Run has ever prepared or opened; initial recording time is 0.
        record.recording_time_ns = 0;
    }
    record.task_id = entry.id;
    record.observed_control_generation = control.control_generation;
    record.active_config_version = control.active_config_version;
    if (record.run_operation) {
        for (auto& pipeline : record.run_operation->authorized_pipelines) {
            pipeline.continuation_permission = false;
        }
    }
    for (auto& pipeline : record.pipelines) {
        pipeline.instance.continuation_permission = false;
    }
    return entry.recovery->publish(record, expected);
}

Result<void> record_ended(task::TaskEntry& entry, const task::RunExecution& execution) {
    std::lock_guard<std::mutex> recovery(entry.recovery_execution);
    auto loaded = entry.recovery->load();
    if (!loaded.ok() && loaded.status().code() == StatusCode::Unavailable) {
        auto actual = entry.recovery->reconcile();
        if (!actual.ok()) {
            return actual.status();
        }
        loaded = actual.value()
                     ? Result<task::TaskRecoveryRecord>(*actual.value())
                     : Result<task::TaskRecoveryRecord>(Status(StatusCode::NotFound, {}));
    }
    if (!loaded.ok()) {
        return loaded.status().code() == StatusCode::NotFound ? Result<void>{}
                                                              : Result<void>(loaded.status());
    }
    auto record = std::move(loaded.value());
    if (!record.run_operation) {
        return {};
    }
    if (record.run_operation->phase == task::RunPhase::Ended) {
        return {};
    }
    if (!execution.control->current_operation_id ||
        !(record.run_operation->operation_id == *execution.control->current_operation_id) ||
        record.record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::Conflict, {});
    }
    for (const auto& authorized : record.run_operation->authorized_pipelines) {
        bool owned = false;
        for (const auto& selected : execution.pipelines) {
            auto facts = selected.controller->snapshot();
            if (!facts.ok()) {
                return facts.status();
            }
            owned = owned || (facts.value().identity.pipeline_id == authorized.pipeline_id &&
                              facts.value().identity.instance_id == authorized.instance_id);
        }
        if (!owned) {
            return Status(StatusCode::Conflict, {});
        }
    }
    auto sample = execution.sample();
    if (!sample.ok()) {
        return sample.status();
    }
    for (auto& pipeline : record.run_operation->authorized_pipelines) {
        if (!pipeline.ended_boundary) {
            pipeline.ended_boundary =
                task::RecoveryBoundary{execution.boot.kernel_uuid, sample.value().monotonic_ns};
        }
    }
    record.run_operation->phase = task::RunPhase::Ended;
    auto recording_time = record.recording_time_ns.value_or(0);
    for (const auto& pipeline : execution.pipelines) {
        auto boundary = pipeline.controller->recording_time_boundary();
        if (!boundary.ok()) {
            return boundary.status();
        }
        if (boundary.value()) {
            recording_time = std::max(recording_time, *boundary.value());
        }
    }
    record.recording_time_ns = recording_time;
    const auto expected = record.record_revision++;
    return entry.recovery->publish(record, expected);
}

}  // namespace

bool TaskService::State::cleanup_failed_prepare(
    const std::shared_ptr<task::TaskEntry>& entry,
    const std::shared_ptr<task::RunExecution>& execution, StatusCode failure) noexcept {
    try {
        auto job = std::make_shared<CloseJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        job->execution = execution;
        job->control = execution->control;
        job->reply = std::make_shared<task::Reply<OperationId>>();
        job->internal_cleanup = true;
        job->preparation_failure = failure;
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            if (entry->execution != execution || entry->closing ||
                !entry->controller->run_is_current(
                    *execution->control->current_operation_id,
                    execution->control->control_generation)) {
                return false;
            }
            ++entry->closing;
            execution->fence();
            std::lock_guard<std::mutex> registered(execution->registration);
            execution->draining = true;
        }
        auto queued = enqueue_cleanup(entry, job);
        if (!queued.ok()) {
            CloseFlight release{*entry, execution};
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void TaskService::State::close_execution(
    const std::shared_ptr<task::TaskEntry>& entry, std::shared_ptr<task::Reply<OperationId>> reply,
    std::shared_ptr<const task::TaskControlRecord> accepted) noexcept {
    try {
        auto job = std::make_shared<CloseJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        job->reply = reply;
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            job->control = std::move(accepted);
            job->execution = entry->execution;
            if (job->execution) {
                job->execution->fence();
                std::lock_guard<std::mutex> lock(job->execution->registration);
                job->execution->draining = true;
            }
        }
        auto queued = enqueue_cleanup(entry, job);
        if (!queued.ok()) {
            CloseFlight release{*entry, job->execution};
            job->reply->complete(queued.status());
        }
    } catch (...) {
        CloseFlight release{*entry, {}};
        if (reply) {
            reply->complete(Status(StatusCode::ResourceExhausted, {}));
        }
    }
}

void TaskService::State::CloseJob::run() noexcept {
    CloseFlight release{*entry, execution};
    StatusCode failure = preparation_failure;
    DataImpact impact = DataImpact::None;
    std::uint64_t accepted = 0;
    const auto fail = [&](StatusCode error) noexcept {
        if (failure == StatusCode::Ok) {
            failure = error;
        }
    };
    try {
        auto revoked = internal_cleanup || process_drain ? Result<void>{}
                                                         : revoke_continuation(*entry, *control);
        if (!revoked.ok()) {
            reply->complete(revoked.status());
            return;
        }
        if (execution) {
            std::lock_guard<std::mutex> registered(execution->registration);
            for (const auto& pipeline : execution->pipelines) {
                auto closed = pipeline.controller->close_admission(control->control_generation);
                if (!closed.ok()) {
                    reply->complete(closed.status());
                    return;
                }
            }
        }
        if (!internal_cleanup && !process_drain) {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->closed_acceptance = *control->current_operation_id;
        }
        if (!internal_cleanup && !process_drain) {
            reply->complete(*control->current_operation_id);
        }
        if (execution) {
            {
                std::unique_lock<std::mutex> lock(execution->registration);
                // Actual prepare return, independent of the caller's observer.
                execution->returned.wait(lock, [&] { return execution->preparation_returned; });
                if (!execution->returned.wait_for(
                        lock, owner->options->execution->native_timeout,
                        [&] { return !execution->rotating_native; })) {
                    reply->complete(Status(StatusCode::DeadlineExceeded, {}));
                    return;
                }
            }
            for (const auto& pipeline : execution->pipelines) {
                auto closed = pipeline.controller->close_admission(control->control_generation);
                if (!closed.ok()) {
                    fail(closed.status().code());
                    continue;
                }
                auto deadline = plugin::Deadline::after(owner->options->execution->native_timeout);
                if (!deadline.ok()) {
                    fail(deadline.status().code());
                    continue;
                }
                // Fixed two-Segment rotation envelope: settle the old handoff,
                // then the final writable Segment, followed by an empty proof.
                // Never loop unboundedly or release a busy/uncertain Commit.
                for (unsigned pass = 0; pass < 3; ++pass) {
                    auto drained = pipeline.controller->drain(deadline.value());
                    if (!drained.ok()) {
                        fail(drained.status().code());
                        break;
                    }
                    if (!drained.value()) {
                        break;
                    }
                    auto committed = owner->options->execution->commit(*drained.value());
                    if (!committed.ok() && committed.status().code() == StatusCode::Conflict) {
                        bool returned = false;
                        {
                            std::unique_lock<std::mutex> registered(execution->registration);
                            if (execution->rotating) {
                                returned = execution->returned.wait_for(
                                    registered, owner->options->execution->native_timeout,
                                    [&] { return !execution->rotating; });
                            }
                        }
                        if (returned) {
                            committed = owner->options->execution->commit(*drained.value());
                        }
                    }
                    if (committed.ok()) {
                        committed = pipeline.controller->acknowledge_artifact(
                            drained.value()->work_id,
                            drained.value()->segment->metadata.segment_id());
                    }
                    if (!committed.ok()) {
                        fail(committed.status().code());
                        impact = DataImpact::PersistenceUnknown;
                        break;
                    }
                }
                auto cleanup = pipeline.controller->cleanup(deadline.value());
                if (!cleanup.ok()) {
                    fail(cleanup.status().code());
                }
                auto facts = pipeline.controller->snapshot();
                if (!facts.ok()) {
                    fail(facts.status().code());
                } else {
                    accepted += facts.value().records.accepted;
                }
                if (facts.ok() && failure != StatusCode::Ok && facts.value().records.written != 0 &&
                    facts.value().artifact_work_id != 0 && impact == DataImpact::None) {
                    impact = DataImpact::PersistenceUnknown;
                }
                if (facts.ok() && (facts.value().records.unknown != 0 ||
                                   facts.value().records.known_failed != 0)) {
                    fail(StatusCode::DataLoss);
                    if (facts.value().records.unknown != 0) {
                        impact = DataImpact::PersistenceUnknown;
                    } else if (impact == DataImpact::None) {
                        impact = DataImpact::AcceptedKnownFailed;
                    }
                }
            }
            {
                std::unique_lock<std::mutex> lock(execution->registration);
                if (!execution->returned.wait_for(
                        lock, owner->options->execution->native_timeout,
                        [&] { return !execution->rotating; })) {
                    fail(StatusCode::DeadlineExceeded);
                }
                execution->draining = false;
                execution->failure = failure;
            }
            const bool settled = execution->resources_settled();
            auto ended = settled ? record_ended(*entry, *execution)
                                 : Result<void>(Status(StatusCode::InvalidState, {}));
            {
                std::lock_guard<std::mutex> registered(execution->registration);
                execution->ended_pending = settled && !ended.ok();
            }
            if (!ended.ok()) {
                fail(ended.status().code());
            }
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            if (entry->execution == execution) {
                entry->quiescent = settled && ended.ok();
            }
        }
        if (internal_cleanup) {
            if (impact == DataImpact::None && accepted == 0) {
                impact = DataImpact::PreAdmissionOnly;
            }
            std::lock_guard<std::mutex> registered(execution->registration);
            execution->result_ready = true;
            execution->failure = failure;
            execution->result_impact = impact;
        }
        release.finish();
        if (process_drain) {
            reply->complete(OperationId{});
            return;
        }
        if (internal_cleanup && execution->purpose != task::RunPurpose::Operation) {
            return;
        }
        owner->finish_execution(
            entry, *control->current_operation_id, control->control_generation, failure, false,
            impact);
    } catch (const std::bad_alloc&) {
        reply->complete(Status(StatusCode::ResourceExhausted, {}));
    } catch (...) {
        reply->complete(Status(StatusCode::Internal, {}));
    }
}

}  // namespace rodaq
