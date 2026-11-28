#include "task_service_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../artifact/artifact_catalog.hpp"
#include "../artifact/volume_capacity.hpp"

namespace rodaq {
namespace {

DataImpact combine_impact(DataImpact left, DataImpact right) noexcept {
    return static_cast<DataImpact>(
        std::max(static_cast<unsigned>(left), static_cast<unsigned>(right)));
}

void commit_sealed_segment(
    const task::RunExecutionOptions& exec_opts,
    const std::shared_ptr<pipeline::PipelineController>& controller,
    std::optional<artifact::SealedSegment>& sealed, StatusCode& failure, DataImpact& impact) {
    const auto work_id = sealed->work_id;
    const auto seg_id = sealed->segment_id;
    auto ack = [controller, work_id, seg_id]() noexcept {
        (void)controller->acknowledge_artifact(work_id, seg_id);
    };
    Result<void> committed = Status(StatusCode::ResourceExhausted, {});
    try {
        committed = exec_opts.commit_with_ack(sealed, ack);
    } catch (const std::bad_alloc&) {
        // The caller still owns the handoff if the callback could not be allocated.
    }
    if (sealed) {
        auto aborted = exec_opts.abort_sealed(*sealed);
        if (aborted.ok()) {
            ack();
        } else {
            committed = aborted.status();
        }
        sealed.reset();
    }
    if (!committed.ok()) {
        if (failure == StatusCode::Ok) {
            failure = committed.status().code();
        }
        impact = combine_impact(impact, DataImpact::PersistenceUnknown);
    }
}

void settle_pipeline_close(
    const task::RunExecutionOptions& exec_opts,
    const std::shared_ptr<pipeline::PipelineController>& controller, ControlGeneration generation,
    plugin::Deadline deadline, StatusCode& failure, DataImpact& impact,
    std::uint64_t& max_recording_boundary) noexcept {
    try {
        (void)controller->close_admission(generation);
        auto boundary = controller->recording_time_boundary();
        if (boundary.ok() && boundary.value().has_value()) {
            max_recording_boundary = std::max(max_recording_boundary, *boundary.value());
        }
        auto drained = controller->drain(deadline);
        if (!drained.ok()) {
            if (failure == StatusCode::Ok) {
                failure = drained.status().code();
            }
            if (drained.status().code() == StatusCode::DataLoss) {
                impact = combine_impact(impact, DataImpact::PersistenceUnknown);
            }
        } else if (drained.value().has_value()) {
            commit_sealed_segment(exec_opts, controller, drained.value(), failure, impact);
        }
        auto cleaned = controller->cleanup(deadline);
        if (!cleaned.ok()) {
            if (failure == StatusCode::Ok) {
                failure = cleaned.status().code();
            }
        }
        auto snap = controller->snapshot();
        if (snap.ok()) {
            if (snap.value().records.unknown != 0 || snap.value().records.unresolved_owners != 0) {
                impact = combine_impact(impact, DataImpact::PersistenceUnknown);
            } else if (snap.value().records.known_failed != 0) {
                impact = combine_impact(impact, DataImpact::AcceptedKnownFailed);
            }
        }
    } catch (...) {
        if (failure == StatusCode::Ok) {
            failure = StatusCode::Internal;
        }
    }
}

}  // namespace

namespace task {

void RunExecution::fence(std::optional<std::uint64_t> monotonic_ns) noexcept {
    std::vector<RunPipeline> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        fenced = true;
        snapshot = pipelines;
    }
    for (const auto& pipe : snapshot) {
        if (pipe.controller) {
            (void)pipe.controller->establish_control_fence(monotonic_ns);
        }
    }
}

bool RunExecution::resources_settled() {
    std::vector<RunPipeline> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (preparing || closing || rotating) {
            return false;
        }
        snapshot = pipelines;
    }
    for (const auto& pipe : snapshot) {
        if (!pipe.controller) {
            continue;
        }
        auto snap = pipe.controller->snapshot();
        if (!snap.ok() || !snap.value().quiescence.settled) {
            return false;
        }
    }
    return true;
}

ExecutionCondition RunExecution::condition(DesiredState desired) {
    std::vector<RunPipeline> snapshot;
    bool is_preparing = false;
    bool is_closing = false;
    bool is_degraded = false;
    bool is_protected = false;
    StatusCode fail = StatusCode::Ok;
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = pipelines;
        is_preparing = preparing;
        is_closing = closing;
        is_degraded = degraded;
        is_protected = volume_protected;
        fail = failure;
    }
    if (is_protected) {
        return ExecutionCondition::Protected;
    }
    if (is_closing) {
        return ExecutionCondition::Draining;
    }
    if (is_preparing) {
        return ExecutionCondition::Preparing;
    }
    bool any_open = false;
    bool any_failed = fail != StatusCode::Ok;
    bool all_settled = true;
    for (const auto& pipe : snapshot) {
        if (!pipe.controller) {
            continue;
        }
        auto snap = pipe.controller->snapshot();
        if (!snap.ok()) {
            any_failed = true;
            all_settled = false;
            continue;
        }
        if (snap.value().gate.open && !snap.value().gate.control_fence) {
            any_open = true;
        }
        if (snap.value().failure != StatusCode::Ok) {
            any_failed = true;
        }
        if (!snap.value().quiescence.settled) {
            all_settled = false;
        }
    }
    if (desired == DesiredState::Running) {
        if (any_open) {
            return (is_degraded || any_failed) ? ExecutionCondition::Degraded
                                               : ExecutionCondition::Active;
        }
        if (any_failed) {
            return all_settled ? ExecutionCondition::Failed : ExecutionCondition::Protected;
        }
        return all_settled ? ExecutionCondition::Idle : ExecutionCondition::Draining;
    }
    if (!all_settled) {
        return any_failed ? ExecutionCondition::Protected : ExecutionCondition::Draining;
    }
    return ExecutionCondition::Idle;
}

}  // namespace task

void TaskService::State::start_prepare(
    const std::shared_ptr<task::TaskEntry>& entry,
    const std::shared_ptr<task::RunExecution>& execution) noexcept {
    struct PrepareJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        std::shared_ptr<task::RunExecution> execution;

        void run() noexcept override {
            struct ActiveGuard {
                std::atomic<std::size_t>& counter;
                ~ActiveGuard() noexcept { counter.fetch_sub(1, std::memory_order_release); }
            } active{owner->active_jobs};

            const auto& exec_opts = *owner->options->execution;
            auto deadline = plugin::Deadline::after(exec_opts.native_timeout);
            StatusCode fail_code = StatusCode::Ok;
            std::string fail_msg;

            // 1. Resolve closed PipelineControllers from Composition.
            Result<task::RunInputs> resolved(Status(StatusCode::Internal, {}));
            if (!deadline.ok()) {
                fail_code = deadline.status().code();
            } else {
                try {
                    resolved = exec_opts.resolve(
                        execution->task_id, execution->config_version,
                        execution->control_generation, *execution->config);
                } catch (...) {
                    resolved = Status(StatusCode::Internal, "Exception in resolve");
                }
                if (!resolved.ok()) {
                    fail_code = resolved.status().code();
                    fail_msg = resolved.status().message();
                }
            }

            if (fail_code == StatusCode::Ok) {
                std::lock_guard<std::mutex> elock(execution->mutex);
                execution->pipelines = std::move(resolved.value().pipelines);
            }

            // 2. Prepare all Pipelines (Fixed Required/Optional Prepare Barrier).
            std::vector<std::optional<pipeline::PreparedPipeline>> prepared_results;
            std::vector<StatusCode> prepare_statuses;
            bool any_required_failed = false;
            bool any_optional_failed = false;
            std::size_t ready_count = 0;

            if (fail_code == StatusCode::Ok) {
                std::vector<task::RunPipeline> pipes;
                {
                    std::lock_guard<std::mutex> elock(execution->mutex);
                    pipes = execution->pipelines;
                }
                prepared_results.resize(pipes.size());
                prepare_statuses.assign(pipes.size(), StatusCode::Ok);

                for (std::size_t i = 0; i < pipes.size(); ++i) {
                    bool is_fenced = false;
                    {
                        std::lock_guard<std::mutex> elock(execution->mutex);
                        is_fenced = execution->fenced;
                    }
                    if (is_fenced) {
                        prepare_statuses[i] = StatusCode::Conflict;
                        if (pipes[i].required) {
                            any_required_failed = true;
                        }
                        continue;
                    }
                    auto res = pipes[i].controller->prepare(deadline.value());
                    if (res.ok()) {
                        prepared_results[i] = res.value();
                        prepare_statuses[i] = StatusCode::Ok;
                        ++ready_count;
                    } else {
                        prepare_statuses[i] = res.status().code();
                        if (pipes[i].required) {
                            any_required_failed = true;
                            if (fail_code == StatusCode::Ok) {
                                fail_code = res.status().code();
                                fail_msg = res.status().message();
                            }
                        } else {
                            any_optional_failed = true;
                        }
                    }
                }
                {
                    std::lock_guard<std::mutex> elock(execution->mutex);
                    for (std::size_t i = 0; i < execution->pipelines.size(); ++i) {
                        execution->pipelines[i].prepare_status = prepare_statuses[i];
                        execution->pipelines[i].active = prepared_results[i].has_value();
                    }
                }
            }

            if (fail_code == StatusCode::Ok && (any_required_failed || ready_count == 0)) {
                fail_code = StatusCode::Unavailable;
                fail_msg = "Prepare barrier failed";
            }

            // 3. Evaluate barrier under Task control lock and Open Admission if current.
            bool opened_all = false;
            bool superseded = false;
            bool need_cleanup = false;
            std::vector<std::shared_ptr<pipeline::PipelineController>> optional_failed_cleanup;

            {
                std::lock_guard<std::mutex> lock(entry->control_mutex);
                bool fenced = false;
                {
                    std::lock_guard<std::mutex> elock(execution->mutex);
                    execution->preparing = false;
                    execution->preparation_returned = true;
                    fenced = execution->fenced;
                }
                if (entry->control_generation != execution->control_generation || fenced) {
                    superseded = true;
                    need_cleanup = true;
                } else if (fail_code == StatusCode::Ok) {
                    auto sample = exec_opts.sample();
                    if (!sample.ok()) {
                        fail_code = sample.status().code();
                        need_cleanup = true;
                    } else {
                        std::lock_guard<std::mutex> elock(execution->mutex);
                        for (std::size_t i = 0; i < execution->pipelines.size(); ++i) {
                            if (!prepared_results[i]) {
                                optional_failed_cleanup.push_back(
                                    execution->pipelines[i].controller);
                                continue;
                            }
                            const auto& prep = *prepared_results[i];
                            const auto open_mono =
                                std::max(sample.value().monotonic_ns, prep.prepared_monotonic);
                            pipeline::OpenAuthorization auth{execution->task_id,
                                                             prep.pipeline_id,
                                                             prep.instance_id,
                                                             prep.control_generation,
                                                             prep.prepared_monotonic,
                                                             open_mono,
                                                             execution->recording_base_ns};
                            const auto open_res =
                                execution->pipelines[i].controller->open_admission(auth);
                            if (open_res != pipeline::GateOpenResult::Opened) {
                                execution->pipelines[i].active = false;
                                if (execution->pipelines[i].required) {
                                    fail_code = StatusCode::Conflict;
                                    need_cleanup = true;
                                    break;
                                }
                                any_optional_failed = true;
                                optional_failed_cleanup.push_back(
                                    execution->pipelines[i].controller);
                            }
                        }
                        if (!need_cleanup) {
                            execution->opened = true;
                            execution->degraded = any_optional_failed;
                            opened_all = true;
                        }
                    }
                } else {
                    need_cleanup = true;
                }

                if (opened_all) {
                    if (entry->current_operation &&
                        entry->current_operation->operation_id == execution->start_operation_id) {
                        entry->current_operation->state = any_optional_failed
                                                              ? OperationState::PartiallySucceeded
                                                              : OperationState::Succeeded;
                        entry->current_operation->terminal_at = task::control_wall_now();
                        entry->current_operation->status_code = StatusCode::Ok;
                        entry->operations.update(*entry->current_operation);
                    }
                    owner->publish_locked(*entry);
                } else if (!superseded) {
                    {
                        std::lock_guard<std::mutex> elock(execution->mutex);
                        execution->failure = fail_code;
                    }
                    if (entry->current_operation &&
                        entry->current_operation->operation_id == execution->start_operation_id) {
                        entry->current_operation->state = OperationState::Failed;
                        entry->current_operation->terminal_at = task::control_wall_now();
                        entry->current_operation->status_code = fail_code;
                        entry->current_operation->failure_domain = FailureDomain::Pipeline;
                        entry->current_operation->retryability = Retryability::AfterConditionChange;
                        entry->current_operation->diagnostic = fail_msg;
                        entry->operations.update(*entry->current_operation);
                    }
                    owner->publish_locked(*entry);
                }
            }

            // Clean up any failed optional pipelines in background.
            for (const auto& opt_ctrl : optional_failed_cleanup) {
                if (!opt_ctrl) {
                    continue;
                }
                auto clean_dl = plugin::Deadline::after(exec_opts.native_timeout);
                if (clean_dl.ok()) {
                    (void)opt_ctrl->establish_control_fence();
                    (void)opt_ctrl->close_admission(execution->control_generation);
                    (void)opt_ctrl->cleanup(clean_dl.value());
                }
            }

            if (need_cleanup) {
                execution->fence();
                if (!superseded) {
                    owner->start_close(
                        entry, execution, std::nullopt, execution->control_generation, false, false,
                        true);
                } else {
                    std::lock_guard<std::mutex> elock(execution->mutex);
                    execution->changed.notify_all();
                }
            }
        }
    };

    try {
        active_jobs.fetch_add(1, std::memory_order_acq_rel);
        auto job = std::make_shared<PrepareJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        job->execution = execution;
        auto submitted = options->execution->preparation_lane->submit(std::move(job));
        if (!submitted.ok()) {
            active_jobs.fetch_sub(1, std::memory_order_release);
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            execution->preparing = false;
            execution->preparation_returned = true;
            execution->failure = submitted.status().code();
            if (entry->current_operation &&
                entry->current_operation->operation_id == execution->start_operation_id) {
                entry->current_operation->state = OperationState::Failed;
                entry->current_operation->terminal_at = task::control_wall_now();
                entry->current_operation->status_code = submitted.status().code();
                entry->operations.update(*entry->current_operation);
            }
            entry->execution.reset();
            publish_locked(*entry);
        }
    } catch (...) {
        active_jobs.fetch_sub(1, std::memory_order_release);
    }
}

void TaskService::State::start_close(
    const std::shared_ptr<task::TaskEntry>& entry,
    const std::shared_ptr<task::RunExecution>& execution, std::optional<OperationId> operation_id,
    ControlGeneration generation, bool is_pause, bool process_drain,
    bool internal_cleanup) noexcept {
    struct CloseJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        std::shared_ptr<task::RunExecution> execution;
        std::optional<OperationId> operation_id;
        ControlGeneration generation{0};
        bool is_pause{false};
        bool process_drain{false};
        bool internal_cleanup{false};

        void run() noexcept override {
            struct ActiveGuard {
                std::atomic<std::size_t>& counter;
                ~ActiveGuard() noexcept { counter.fetch_sub(1, std::memory_order_release); }
            } active{owner->active_jobs};

            const auto& exec_opts = *owner->options->execution;
            execution->fence();

            // Wait for any in-flight PrepareJob to finish before touching controllers.
            {
                std::unique_lock<std::mutex> elock(execution->mutex);
                if (execution->closing) {
                    return;
                }
                execution->closing = true;
                execution->changed.wait_for(elock, exec_opts.native_timeout, [&] {
                    return execution->preparation_returned || !execution->preparing;
                });
            }

            std::vector<task::RunPipeline> pipes;
            {
                std::lock_guard<std::mutex> elock(execution->mutex);
                pipes = execution->pipelines;
            }

            auto deadline = plugin::Deadline::after(exec_opts.native_timeout);
            StatusCode aggregate_failure = StatusCode::Ok;
            DataImpact aggregate_impact = DataImpact::None;
            std::uint64_t max_boundary = execution->recording_base_ns;

            if (!deadline.ok()) {
                aggregate_failure = deadline.status().code();
            } else if (pipes.size() > 1 && exec_opts.pipeline_cleanup_lane) {
                // Parallel multi-Pipeline drain/seal/commit/cleanup (IC-T04).
                struct Latch {
                    std::mutex mutex;
                    std::condition_variable done;
                    std::size_t remaining{0};
                    StatusCode failure{StatusCode::Ok};
                    DataImpact impact{DataImpact::None};
                    std::uint64_t max_boundary{0};
                };
                auto latch = std::make_shared<Latch>();
                latch->remaining = pipes.size();
                latch->max_boundary = max_boundary;

                struct PipeCloseWork final : Work {
                    const task::RunExecutionOptions* exec_opts{nullptr};
                    std::shared_ptr<pipeline::PipelineController> controller;
                    ControlGeneration generation{0};
                    plugin::Deadline deadline{plugin::Deadline::infinite()};
                    std::shared_ptr<Latch> latch;

                    void run() noexcept override {
                        StatusCode local_fail = StatusCode::Ok;
                        DataImpact local_impact = DataImpact::None;
                        std::uint64_t local_boundary = 0;
                        if (controller) {
                            settle_pipeline_close(
                                *exec_opts, controller, generation, deadline, local_fail,
                                local_impact, local_boundary);
                        }
                        std::lock_guard<std::mutex> lock(latch->mutex);
                        if (local_fail != StatusCode::Ok && latch->failure == StatusCode::Ok) {
                            latch->failure = local_fail;
                        }
                        latch->impact = combine_impact(latch->impact, local_impact);
                        latch->max_boundary = std::max(latch->max_boundary, local_boundary);
                        if (--latch->remaining == 0) {
                            latch->done.notify_all();
                        }
                    }
                };

                std::size_t submitted_count = 0;
                for (const auto& pipe : pipes) {
                    auto sub = std::make_shared<PipeCloseWork>();
                    sub->exec_opts = &exec_opts;
                    sub->controller = pipe.controller;
                    sub->generation = generation;
                    sub->deadline = deadline.value();
                    sub->latch = latch;
                    auto queued = exec_opts.pipeline_cleanup_lane->submit(std::move(sub));
                    if (!queued.ok()) {
                        if (pipe.controller) {
                            settle_pipeline_close(
                                exec_opts, pipe.controller, generation, deadline.value(),
                                aggregate_failure, aggregate_impact, max_boundary);
                        }
                        std::lock_guard<std::mutex> lock(latch->mutex);
                        if (--latch->remaining == 0) {
                            latch->done.notify_all();
                        }
                    } else {
                        ++submitted_count;
                    }
                }
                if (submitted_count > 0) {
                    std::unique_lock<std::mutex> lock(latch->mutex);
                    if (!latch->done.wait_for(lock, exec_opts.native_timeout * 2, [&] {
                            return latch->remaining == 0;
                        })) {
                        if (aggregate_failure == StatusCode::Ok) {
                            aggregate_failure = StatusCode::DeadlineExceeded;
                        }
                    }
                    if (latch->failure != StatusCode::Ok && aggregate_failure == StatusCode::Ok) {
                        aggregate_failure = latch->failure;
                    }
                    aggregate_impact = combine_impact(aggregate_impact, latch->impact);
                    max_boundary = std::max(max_boundary, latch->max_boundary);
                }
            } else {
                for (const auto& pipe : pipes) {
                    if (pipe.controller) {
                        settle_pipeline_close(
                            exec_opts, pipe.controller, generation, deadline.value(),
                            aggregate_failure, aggregate_impact, max_boundary);
                    }
                }
            }

            {
                std::lock_guard<std::mutex> elock(execution->mutex);
                execution->closing = false;
                if (aggregate_failure != StatusCode::Ok && execution->failure == StatusCode::Ok) {
                    execution->failure = aggregate_failure;
                }
                execution->data_impact = combine_impact(execution->data_impact, aggregate_impact);
            }
            const bool settled = execution->resources_settled();

            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->last_data_impact = combine_impact(entry->last_data_impact, aggregate_impact);
            if (settled) {
                if (is_pause) {
                    entry->recording_base_ns = max_boundary;
                } else if (!process_drain && !internal_cleanup) {
                    entry->recording_base_ns = 0;
                }
                if (entry->execution == execution) {
                    entry->execution.reset();
                }
            }
            if (operation_id && entry->current_operation &&
                entry->current_operation->operation_id == *operation_id &&
                !task::is_terminal(entry->current_operation->state)) {
                entry->current_operation->terminal_at = task::control_wall_now();
                entry->current_operation->data_impact =
                    combine_impact(entry->current_operation->data_impact, aggregate_impact);
                if (aggregate_failure == StatusCode::Ok && settled) {
                    entry->current_operation->state = OperationState::Succeeded;
                    entry->current_operation->status_code = StatusCode::Ok;
                } else if (settled && aggregate_impact == DataImpact::AcceptedKnownFailed) {
                    entry->current_operation->state = OperationState::PartiallySucceeded;
                    entry->current_operation->status_code = aggregate_failure;
                } else {
                    entry->current_operation->state = OperationState::Failed;
                    entry->current_operation->status_code = aggregate_failure != StatusCode::Ok
                                                                ? aggregate_failure
                                                                : StatusCode::Unavailable;
                    entry->current_operation->failure_domain = FailureDomain::Pipeline;
                    entry->current_operation->retryability = Retryability::AfterProcessRestart;
                }
                entry->operations.update(*entry->current_operation);
            }
            owner->publish_locked(*entry);
        }
    };

    try {
        active_jobs.fetch_add(1, std::memory_order_acq_rel);
        auto job = std::make_shared<CloseJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        job->execution = execution;
        job->operation_id = operation_id;
        job->generation = generation;
        job->is_pause = is_pause;
        job->process_drain = process_drain;
        job->internal_cleanup = internal_cleanup;
        auto submitted = options->execution->cleanup_lane->submit(std::move(job));
        if (!submitted.ok()) {
            active_jobs.fetch_sub(1, std::memory_order_release);
        }
    } catch (...) {
        active_jobs.fetch_sub(1, std::memory_order_release);
    }
}

void TaskService::State::start_reset(
    const std::shared_ptr<task::TaskEntry>& entry, OperationId operation_id,
    ControlGeneration generation) noexcept {
    struct ResetJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        OperationId operation_id;
        ControlGeneration generation{0};

        void run() noexcept override {
            struct ActiveGuard {
                std::atomic<std::size_t>& counter;
                ~ActiveGuard() noexcept { counter.fetch_sub(1, std::memory_order_release); }
            } active{owner->active_jobs};

            const auto& artifacts = owner->options->execution->artifacts;
            StatusCode fail = StatusCode::Ok;
            std::size_t deleted_count = 0;
            std::size_t failed_count = 0;

            if (!artifacts.reset_targets || !artifacts.delete_for_reset) {
                fail = StatusCode::Unavailable;
            } else {
                auto targets = artifacts.reset_targets(entry->id);
                if (!targets.ok()) {
                    fail = targets.status().code();
                } else {
                    const auto deadline = std::chrono::steady_clock::now() +
                                          owner->options->execution->native_timeout;
                    for (const auto id : targets.value()) {
                        auto res = artifacts.delete_for_reset(id, deadline);
                        if (res.ok() || res.status().code() == StatusCode::NotFound) {
                            ++deleted_count;
                        } else {
                            ++failed_count;
                            if (fail == StatusCode::Ok) {
                                fail = res.status().code();
                            }
                        }
                    }
                }
            }

            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->resetting = false;
            entry->recording_base_ns = 0;
            entry->last_data_impact = DataImpact::None;
            if (entry->current_operation &&
                entry->current_operation->operation_id == operation_id &&
                !task::is_terminal(entry->current_operation->state)) {
                entry->current_operation->terminal_at = task::control_wall_now();
                entry->current_operation->data_impact = DataImpact::None;
                if (fail == StatusCode::Ok && failed_count == 0) {
                    entry->current_operation->state = OperationState::Succeeded;
                    entry->current_operation->status_code = StatusCode::Ok;
                } else if (deleted_count > 0 && failed_count > 0) {
                    entry->current_operation->state = OperationState::PartiallySucceeded;
                    entry->current_operation->status_code = fail;
                    entry->current_operation->failure_domain = FailureDomain::Volume;
                    entry->current_operation->retryability = Retryability::AfterConditionChange;
                } else {
                    entry->current_operation->state = OperationState::Failed;
                    entry->current_operation->status_code =
                        fail != StatusCode::Ok ? fail : StatusCode::Unavailable;
                    entry->current_operation->failure_domain = FailureDomain::Volume;
                    entry->current_operation->retryability = Retryability::AfterConditionChange;
                }
                entry->operations.update(*entry->current_operation);
            }
            owner->publish_locked(*entry);
        }
    };

    try {
        active_jobs.fetch_add(1, std::memory_order_acq_rel);
        auto job = std::make_shared<ResetJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        job->operation_id = operation_id;
        job->generation = generation;
        auto submitted = options->execution->cleanup_lane->submit(std::move(job));
        if (!submitted.ok()) {
            active_jobs.fetch_sub(1, std::memory_order_release);
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->resetting = false;
        }
    } catch (...) {
        active_jobs.fetch_sub(1, std::memory_order_release);
    }
}

void TaskService::State::start_retire(
    const std::shared_ptr<task::TaskEntry>& entry, OperationId operation_id,
    ControlGeneration generation) noexcept {
    struct RetireJob final : Work {
        std::shared_ptr<State> owner;
        std::shared_ptr<task::TaskEntry> entry;
        OperationId operation_id;
        ControlGeneration generation{0};

        void run() noexcept override {
            struct ActiveGuard {
                std::atomic<std::size_t>& counter;
                ~ActiveGuard() noexcept { counter.fetch_sub(1, std::memory_order_release); }
            } active{owner->active_jobs};

            const auto& exec_opts = *owner->options->execution;
            StatusCode fail = StatusCode::Ok;
            if (!exec_opts.artifacts.retire_eligible) {
                fail = StatusCode::Unavailable;
            } else {
                auto eligible = exec_opts.artifacts.retire_eligible(entry->id);
                if (!eligible.ok()) {
                    fail = eligible.status().code();
                } else if (!eligible.value()) {
                    fail = StatusCode::Conflict;
                }
            }
            if (fail == StatusCode::Ok) {
                std::lock_guard<std::mutex> slock(owner->store_mutex);
                auto removed = owner->store->remove_task(entry->id);
                if (!removed.ok()) {
                    fail = removed.status().code();
                }
            }

            if (fail == StatusCode::Ok) {
                {
                    std::lock_guard<std::mutex> lock(entry->control_mutex);
                    entry->retiring = false;
                    entry->retired = true;
                    if (entry->current_operation &&
                        entry->current_operation->operation_id == operation_id) {
                        entry->current_operation->state = OperationState::Succeeded;
                        entry->current_operation->terminal_at = task::control_wall_now();
                        entry->current_operation->status_code = StatusCode::Ok;
                        entry->operations.update(*entry->current_operation);
                    }
                    owner->publish_locked(*entry);
                }
                {
                    std::lock_guard<std::mutex> clock(owner->catalog_mutex);
                    owner->entries.erase(
                        std::remove(owner->entries.begin(), owner->entries.end(), entry),
                        owner->entries.end());
                }
                // Release per-Task Writer lane slice (IC-T01).
                if (exec_opts.release_task_resources) {
                    exec_opts.release_task_resources(entry->id);
                }
            } else {
                std::lock_guard<std::mutex> lock(entry->control_mutex);
                entry->retiring = false;
                if (entry->current_operation &&
                    entry->current_operation->operation_id == operation_id) {
                    entry->current_operation->state = OperationState::Failed;
                    entry->current_operation->terminal_at = task::control_wall_now();
                    entry->current_operation->status_code = fail;
                    entry->current_operation->failure_domain = FailureDomain::TaskControl;
                    entry->current_operation->retryability = Retryability::AfterConditionChange;
                    entry->operations.update(*entry->current_operation);
                }
                owner->publish_locked(*entry);
            }
        }
    };

    try {
        active_jobs.fetch_add(1, std::memory_order_acq_rel);
        auto job = std::make_shared<RetireJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        job->operation_id = operation_id;
        job->generation = generation;
        auto submitted = options->execution->cleanup_lane->submit(std::move(job));
        if (!submitted.ok()) {
            active_jobs.fetch_sub(1, std::memory_order_release);
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->retiring = false;
        }
    } catch (...) {
        active_jobs.fetch_sub(1, std::memory_order_release);
    }
}

Result<void> TaskService::close_admission() {
    state_->accepting.store(false, std::memory_order_release);
    std::vector<std::shared_ptr<task::TaskEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->catalog_mutex);
        snapshot = state_->entries;
    }
    for (const auto& entry : snapshot) {
        std::shared_ptr<task::RunExecution> exec;
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            exec = entry->execution;
        }
        if (exec) {
            exec->fence();
        }
    }
    return {};
}

Result<void> TaskService::drain_shutdown(bool pending_only) {
    std::vector<std::shared_ptr<task::TaskEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->catalog_mutex);
        snapshot = state_->entries;
    }
    for (const auto& entry : snapshot) {
        std::shared_ptr<task::RunExecution> exec;
        ControlGeneration gen = 0;
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            if (!entry->execution) {
                continue;
            }
            std::lock_guard<std::mutex> elock(entry->execution->mutex);
            if (pending_only && !entry->execution->fenced) {
                continue;
            }
            if (entry->execution->closing) {
                continue;
            }
            exec = entry->execution;
            gen = entry->control_generation;
        }
        if (exec) {
            exec->fence();
            state_->start_close(entry, exec, std::nullopt, gen, false, !pending_only, pending_only);
        }
    }
    return {};
}

bool TaskService::shutdown_settled() const {
    if (state_->requests.load(std::memory_order_acquire) != 0 ||
        state_->active_jobs.load(std::memory_order_acquire) != 0) {
        return false;
    }
    std::vector<std::shared_ptr<task::TaskEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->catalog_mutex);
        snapshot = state_->entries;
    }
    for (const auto& entry : snapshot) {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        if (entry->configuring || entry->resetting || entry->retiring) {
            return false;
        }
        if (entry->execution && !entry->execution->resources_settled()) {
            return false;
        }
    }
    return true;
}

Result<void> TaskService::refresh_execution_facts(
    bool protect, std::optional<std::uint64_t> fence_monotonic_ns) {
    std::vector<std::shared_ptr<task::TaskEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->catalog_mutex);
        snapshot = state_->entries;
    }
    for (const auto& entry : snapshot) {
        std::shared_ptr<task::RunExecution> exec;
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            exec = entry->execution;
            if (protect && exec) {
                std::lock_guard<std::mutex> elock(exec->mutex);
                exec->volume_protected = true;
            }
        }
        if (protect && exec) {
            exec->fence(fence_monotonic_ns);
        }
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        if (entry->execution && entry->execution->resources_settled() &&
            entry->desired_state != DesiredState::Running) {
            entry->execution.reset();
        }
        state_->publish_locked(*entry);
    }
    return {};
}

Result<void> TaskService::schedule_rotation(std::uint64_t now_monotonic) {
    if (!state_->options->execution || !state_->options->execution->allocate_segment_id) {
        return {};
    }
    std::vector<std::shared_ptr<task::TaskEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->catalog_mutex);
        snapshot = state_->entries;
    }
    for (const auto& entry : snapshot) {
        std::shared_ptr<task::RunExecution> exec;
        std::shared_ptr<const TaskConfig> cfg;
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            if (entry->desired_state != DesiredState::Running || !entry->execution) {
                continue;
            }
            exec = entry->execution;
            cfg = entry->config;
        }
        bool due = false;
        {
            std::lock_guard<std::mutex> elock(exec->mutex);
            if (!exec->opened || exec->fenced || exec->closing || exec->rotating) {
                continue;
            }
            for (const auto& pipe : exec->pipelines) {
                if (!pipe.active || !pipe.controller) {
                    continue;
                }
                auto check = pipe.controller->rotation_due(cfg->segment_policy, now_monotonic);
                if (check.ok() && check.value()) {
                    due = true;
                    break;
                }
            }
            if (due) {
                exec->rotating = true;
            }
        }
        if (!due) {
            continue;
        }

        struct RotateJob final : Work {
            std::shared_ptr<State> owner;
            std::shared_ptr<task::TaskEntry> entry;
            std::shared_ptr<task::RunExecution> execution;
            std::shared_ptr<const TaskConfig> cfg;
            std::uint64_t now_monotonic{0};

            void run() noexcept override {
                struct ActiveGuard {
                    std::atomic<std::size_t>& counter;
                    std::shared_ptr<task::RunExecution> exec;
                    ~ActiveGuard() noexcept {
                        {
                            std::lock_guard<std::mutex> elock(exec->mutex);
                            exec->rotating = false;
                        }
                        counter.fetch_sub(1, std::memory_order_release);
                    }
                } guard{owner->active_jobs, execution};

                const auto& exec_opts = *owner->options->execution;
                std::vector<task::RunPipeline> pipes;
                {
                    std::lock_guard<std::mutex> elock(execution->mutex);
                    pipes = execution->pipelines;
                }
                for (const auto& pipe : pipes) {
                    if (!pipe.active || !pipe.controller) {
                        continue;
                    }
                    auto check = pipe.controller->rotation_due(cfg->segment_policy, now_monotonic);
                    if (!check.ok() || !check.value()) {
                        continue;
                    }
                    auto next_id = exec_opts.allocate_segment_id();
                    if (!next_id.ok()) {
                        continue;
                    }
                    auto dl = plugin::Deadline::after(exec_opts.native_timeout);
                    if (!dl.ok()) {
                        continue;
                    }
                    auto rotated = pipe.controller->rotate(next_id.value(), dl.value());
                    if (rotated.ok() && rotated.value().has_value()) {
                        StatusCode fail = StatusCode::Ok;
                        DataImpact impact = DataImpact::None;
                        commit_sealed_segment(
                            exec_opts, pipe.controller, rotated.value(), fail, impact);
                        if (impact != DataImpact::None) {
                            std::lock_guard<std::mutex> lock(entry->control_mutex);
                            entry->last_data_impact =
                                combine_impact(entry->last_data_impact, impact);
                        }
                    }
                }
                std::lock_guard<std::mutex> lock(entry->control_mutex);
                owner->publish_locked(*entry);
            }
        };

        try {
            state_->active_jobs.fetch_add(1, std::memory_order_acq_rel);
            auto job = std::make_shared<RotateJob>();
            job->owner = state_;
            job->entry = entry;
            job->execution = exec;
            job->cfg = cfg;
            job->now_monotonic = now_monotonic;
            auto submitted = state_->options->execution->preparation_lane->submit(std::move(job));
            if (!submitted.ok()) {
                std::lock_guard<std::mutex> elock(exec->mutex);
                exec->rotating = false;
                state_->active_jobs.fetch_sub(1, std::memory_order_release);
            }
        } catch (...) {
            std::lock_guard<std::mutex> elock(exec->mutex);
            exec->rotating = false;
            state_->active_jobs.fetch_sub(1, std::memory_order_release);
        }
    }
    return {};
}

Result<void> TaskService::reconcile_volume(
    artifact::VolumeSpaceCondition condition, std::uint64_t now_monotonic) {
    const bool stopped = condition == artifact::VolumeSpaceCondition::Stopped ||
                         condition == artifact::VolumeSpaceCondition::Unknown;
    if (!stopped) {
        return {};
    }
    (void)refresh_execution_facts(true, now_monotonic);
    return drain_shutdown(true);
}

Result<void> TaskService::apply_retention(artifact::ArtifactCatalog& catalog) {
    std::vector<std::shared_ptr<task::TaskEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(state_->catalog_mutex);
        snapshot = state_->entries;
    }
    for (const auto& entry : snapshot) {
        RetentionPolicy policy;
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            if (!entry->config || entry->resetting || entry->retiring || entry->retired) {
                continue;
            }
            policy = entry->config->retention_policy;
        }
        if (policy.mode != RetentionMode::Rolling) {
            continue;
        }
        // Bounded per-pass deletions so observe() stays responsive.
        constexpr std::size_t kMaxDeletesPerPass = 16;
        for (std::size_t step = 0; step < kMaxDeletesPerPass; ++step) {
            auto selection = catalog.select_retention(entry->id, policy, 0);
            if (!selection.ok() || !selection.value().candidate) {
                break;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            auto deleted = catalog.delete_artifact(*selection.value().candidate, deadline);
            if (!deleted.ok()) {
                break;
            }
        }
    }
    return {};
}

}  // namespace rodaq
