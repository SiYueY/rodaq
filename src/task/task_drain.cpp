#include "task_service_state.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

struct PipelineDrainOutcome {
    StatusCode failure{StatusCode::Ok};
    DataImpact impact{DataImpact::None};
    std::uint64_t accepted{0};
    bool done{false};
};

DataImpact merge_impact(DataImpact current, DataImpact incoming) noexcept {
    if (current == DataImpact::PersistenceUnknown || incoming == DataImpact::PersistenceUnknown) {
        return DataImpact::PersistenceUnknown;
    }
    if (current == DataImpact::AcceptedKnownFailed || incoming == DataImpact::AcceptedKnownFailed) {
        return DataImpact::AcceptedKnownFailed;
    }
    if (current == DataImpact::PreAdmissionOnly || incoming == DataImpact::PreAdmissionOnly) {
        return DataImpact::PreAdmissionOnly;
    }
    return DataImpact::None;
}

PipelineDrainOutcome drain_single_pipeline(
    const task::RunExecutionOptions& exec_opts,
    const std::shared_ptr<task::RunExecution>& execution,
    const std::shared_ptr<pipeline::PipelineController>& controller,
    ControlGeneration control_generation) {
    PipelineDrainOutcome out;
    const auto fail = [&](StatusCode error) noexcept {
        if (out.failure == StatusCode::Ok) {
            out.failure = error;
        }
    };
    auto closed = controller->close_admission(control_generation);
    if (!closed.ok()) {
        fail(closed.status().code());
        out.done = true;
        return out;
    }
    auto deadline = plugin::Deadline::after(exec_opts.native_timeout);
    if (!deadline.ok()) {
        fail(deadline.status().code());
        out.done = true;
        return out;
    }
    // Fixed two-Segment rotation envelope: settle the old handoff, then the
    // final writable Segment, followed by an empty proof. Never loop
    // unboundedly or release a busy/uncertain Commit.
    for (unsigned pass = 0; pass < 3; ++pass) {
        auto drained = controller->drain(deadline.value());
        if (!drained.ok()) {
            fail(drained.status().code());
            break;
        }
        if (!drained.value()) {
            break;
        }
        const auto work_id = drained.value()->work_id;
        const auto segment_id = drained.value()->segment->metadata.segment_id();
        auto do_commit = [&]() -> Result<void> {
            if (exec_opts.commit_with_ack) {
                return exec_opts.commit_with_ack(
                    *drained.value(), [controller, work_id, segment_id] {
                        (void)controller->acknowledge_artifact(work_id, segment_id);
                    });
            }
            return exec_opts.commit(*drained.value());
        };
        auto committed = do_commit();
        if (!committed.ok() && committed.status().code() == StatusCode::Conflict) {
            bool returned = false;
            {
                std::unique_lock<std::mutex> registered(execution->registration);
                if (execution->rotating) {
                    returned = execution->returned.wait_for(
                        registered, exec_opts.native_timeout, [&] { return !execution->rotating; });
                }
            }
            if (returned) {
                committed = do_commit();
            }
        }
        if (committed.ok()) {
            committed = controller->acknowledge_artifact(work_id, segment_id);
        }
        if (!committed.ok()) {
            fail(committed.status().code());
            out.impact = DataImpact::PersistenceUnknown;
            break;
        }
    }
    auto cleanup = controller->cleanup(deadline.value());
    if (!cleanup.ok()) {
        fail(cleanup.status().code());
    }
    auto facts = controller->snapshot();
    if (!facts.ok()) {
        fail(facts.status().code());
    } else {
        out.accepted = facts.value().records.accepted;
    }
    if (facts.ok() && out.failure != StatusCode::Ok && facts.value().records.written != 0 &&
        facts.value().artifact_work_id != 0 && out.impact == DataImpact::None) {
        out.impact = DataImpact::PersistenceUnknown;
    }
    if (facts.ok() &&
        (facts.value().records.unknown != 0 || facts.value().records.known_failed != 0)) {
        fail(StatusCode::DataLoss);
        if (facts.value().records.unknown != 0) {
            out.impact = DataImpact::PersistenceUnknown;
        } else if (out.impact == DataImpact::None) {
            out.impact = DataImpact::AcceptedKnownFailed;
        }
    }
    out.done = true;
    return out;
}

struct ParallelDrainState {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<PipelineDrainOutcome> outcomes;
    std::size_t completed{0};
};

struct PipelineDrainWork final : Work {
    std::shared_ptr<const task::ServiceOptions> options;
    std::shared_ptr<task::RunExecution> execution;
    std::shared_ptr<pipeline::PipelineController> controller;
    ControlGeneration control_generation{0};
    std::shared_ptr<ParallelDrainState> state;
    std::size_t index{0};

    void run() noexcept override {
        PipelineDrainOutcome outcome;
        try {
            outcome = drain_single_pipeline(
                *options->execution, execution, controller, control_generation);
        } catch (const std::bad_alloc&) {
            outcome.failure = StatusCode::ResourceExhausted;
            outcome.impact = DataImpact::PersistenceUnknown;
            outcome.done = true;
        } catch (...) {
            outcome.failure = StatusCode::Internal;
            outcome.impact = DataImpact::PersistenceUnknown;
            outcome.done = true;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        state->outcomes[index] = outcome;
        ++state->completed;
        state->ready.notify_all();
    }
};

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
            const auto& exec_opts = *owner->options->execution;
            if (execution->pipelines.size() > 1 && exec_opts.pipeline_cleanup_lane) {
                const auto count = execution->pipelines.size();
                auto shared = std::make_shared<ParallelDrainState>();
                shared->outcomes.resize(count);
                std::vector<std::size_t> fallback;
                for (std::size_t i = 0; i < count; ++i) {
                    auto work = std::make_shared<PipelineDrainWork>();
                    work->options = owner->options;
                    work->execution = execution;
                    work->controller = execution->pipelines[i].controller;
                    work->control_generation = control->control_generation;
                    work->state = shared;
                    work->index = i;
                    if (!exec_opts.pipeline_cleanup_lane->submit(std::move(work)).ok()) {
                        fallback.push_back(i);
                    }
                }
                for (const auto i : fallback) {
                    auto outcome = drain_single_pipeline(
                        exec_opts, execution, execution->pipelines[i].controller,
                        control->control_generation);
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    shared->outcomes[i] = outcome;
                    ++shared->completed;
                    shared->ready.notify_all();
                }
                const auto grace =
                    std::min(exec_opts.native_timeout, std::chrono::nanoseconds{200000000});
                const auto wait_budget = exec_opts.native_timeout + grace;
                std::unique_lock<std::mutex> lock(shared->mutex);
                (void)shared->ready.wait_for(
                    lock, wait_budget, [&] { return shared->completed == count; });
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& outcome = shared->outcomes[i];
                    if (!outcome.done) {
                        // Pipeline still executing on pipeline_cleanup_lane;
                        // PipelineDrainWork retains its controller backing.
                        fail(StatusCode::DeadlineExceeded);
                        impact = merge_impact(impact, DataImpact::PersistenceUnknown);
                        continue;
                    }
                    if (outcome.failure != StatusCode::Ok) {
                        fail(outcome.failure);
                    }
                    impact = merge_impact(impact, outcome.impact);
                    accepted += outcome.accepted;
                }
            } else {
                for (const auto& pipeline : execution->pipelines) {
                    const auto outcome = drain_single_pipeline(
                        exec_opts, execution, pipeline.controller, control->control_generation);
                    if (outcome.failure != StatusCode::Ok) {
                        fail(outcome.failure);
                    }
                    impact = merge_impact(impact, outcome.impact);
                    accepted += outcome.accepted;
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
