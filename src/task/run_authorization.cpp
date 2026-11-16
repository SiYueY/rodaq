#include "run_authorization.hpp"

#include "../persistence/json.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq::task {
namespace {

const OperationSnapshot* run_operation(const TaskControlRecord& control) noexcept {
    if (!control.current_operation_id || control.desired_state != DesiredState::Running ||
        control.reset_intent) {
        return nullptr;
    }
    for (const auto& entry : control.operation_history.entries) {
        const auto& operation = entry.snapshot;
        if (operation.operation_id == *control.current_operation_id &&
            operation.control_generation == control.control_generation &&
            (operation.kind == OperationKind::Start || operation.kind == OperationKind::Resume) &&
            operation.state != OperationState::Failed &&
            operation.state != OperationState::Superseded) {
            return &operation;
        }
    }
    return nullptr;
}

}  // namespace

bool RunAuthorization::current(const TaskControlRecord& control) const noexcept {
    const auto* operation = run_operation(control);
    return operation && control.task_id == task_id_ && control.control_generation == generation_ &&
           control.active_config_version == config_version_ &&
           operation->operation_id == operation_id_;
}

Result<std::unique_ptr<RunAuthorization>> RunAuthorization::create(
    const TaskControlRecord& accepted, StartPolicyMode policy, std::vector<RunPipeline> pipelines,
    std::shared_ptr<TaskRecoveryStore> recovery, time::BootIdentity boot,
    pipeline::ReceiveClock sample) {
    const auto* operation = run_operation(accepted);
    auto identity = time::parse_boot_identity(boot.kernel_uuid);
    if (!operation || !recovery || !sample || pipelines.empty() || pipelines.size() > 64 ||
        !identity.ok() || identity.value().id != boot.id ||
        (policy != StartPolicyMode::AllRequired && policy != StartPolicyMode::BestEffort)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto result = std::unique_ptr<RunAuthorization>(new RunAuthorization);
        result->task_id_ = accepted.task_id;
        result->operation_id_ = operation->operation_id;
        result->generation_ = accepted.control_generation;
        result->config_version_ = accepted.active_config_version;
        result->policy_ = policy;
        result->pipelines_ = std::move(pipelines);
        result->prepared_.resize(result->pipelines_.size());
        result->recovery_ = std::move(recovery);
        result->boot_ = std::move(boot);
        result->sample_ = sample;
        for (std::size_t i = 0; i < result->pipelines_.size(); ++i) {
            const auto& selected = result->pipelines_[i];
            if (!selected.controller || !persistence::valid_digest(selected.effective_config_id) ||
                !selected.controller->matches_configuration(
                    accepted.active_config_version, selected.effective_config_id)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            auto state = selected.controller->snapshot();
            if (!state.ok()) {
                return state.status();
            }
            const auto& owner = state.value().identity;
            if (!(owner.task_id == accepted.task_id) ||
                owner.control_generation != accepted.control_generation) {
                return Status(StatusCode::InvalidArgument, {});
            }
            for (std::size_t j = 0; j < i; ++j) {
                auto previous = result->pipelines_[j].controller->snapshot();
                if (!previous.ok()) {
                    return previous.status();
                }
                if (previous.value().identity.pipeline_id == owner.pipeline_id ||
                    previous.value().identity.instance_id == owner.instance_id) {
                    return Status(StatusCode::InvalidArgument, {});
                }
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<RunPrepareResult> RunAuthorization::prepare(plugin::Deadline deadline) {
    // The actual Run work domain serializes prepare/publication. Only fence and
    // Open can enter concurrently, through the short opening cut below.
    {
        std::lock_guard<std::mutex> lock(opening_);
        if (fenced_ || prepare_attempted_) {
            return Status(StatusCode::InvalidState, {});
        }
        prepare_attempted_ = true;
    }
    try {
        RunPrepareResult result;
        result.pipelines.resize(pipelines_.size(), StatusCode::InvalidState);
        bool required_ready = true;
        bool any_ready = false;
        for (std::size_t i = 0; i < pipelines_.size(); ++i) {
            auto actual = pipelines_[i].controller->prepare(deadline);
            result.pipelines[i] = actual.ok() ? StatusCode::Ok : actual.status().code();
            if (actual.ok()) {
                prepared_[i] = actual.value();
                any_ready = true;
            } else if (pipelines_[i].required) {
                required_ready = false;
            }
        }
        std::lock_guard<std::mutex> lock(opening_);
        barrier_passed_ =
            !fenced_ && any_ready && (policy_ == StartPolicyMode::BestEffort || required_ready);
        result.barrier_passed = barrier_passed_;
        return result;
    } catch (const std::bad_alloc&) {
        (void)fence();
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> RunAuthorization::checkpoint_prepared() {
    if (prepared_record_) {
        return {};
    }
    try {
        auto previous = recovery_->load();
        if (!previous.ok() && previous.status().code() != StatusCode::NotFound) {
            return previous.status();
        }
        TaskRecoveryRecord record;
        std::uint64_t expected = 0;
        if (previous.ok()) {
            record = std::move(previous.value());
            if (record.run_operation && record.run_operation->phase != RunPhase::Ended) {
                return Status(StatusCode::InvalidState, {});
            }
            if (record.record_revision >= std::numeric_limits<std::uint64_t>::max() - 1 ||
                record.observed_control_generation > generation_ ||
                !record.cleanup_intents.empty() || record.reset_progress) {
                return Status(StatusCode::InvalidState, {});
            }
            expected = record.record_revision++;
        }
        record.task_id = task_id_;
        record.observed_control_generation = generation_;
        record.active_config_version = config_version_;
        RunRecovery run;
        run.operation_id = operation_id_;
        run.control_generation = generation_;
        for (std::size_t i = 0; i < pipelines_.size(); ++i) {
            if (prepared_[i]) {
                const auto& prepared = *prepared_[i];
                if (prepared.boot_id != boot_.id) {
                    return Status(StatusCode::InvalidState, {});
                }
                run.authorized_pipelines.push_back(
                    {prepared.pipeline_id,
                     prepared.instance_id,
                     pipelines_[i].effective_config_id,
                     {boot_.kernel_uuid, prepared.prepared_monotonic},
                     true,
                     false,
                     {}});
            }
        }
        if (run.authorized_pipelines.empty()) {
            return {};
        }
        record.run_operation = std::move(run);
        prepared_record_.emplace(std::move(record));
        auto published = recovery_->publish(*prepared_record_, expected);
        if (!published.ok()) {
            prepared_record_.reset();
            return published.status();
        }
        return {};
    } catch (const std::bad_alloc&) {
        (void)fence();
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> RunAuthorization::publish_authorization(std::uint64_t recording_base) {
    {
        std::lock_guard<std::mutex> lock(opening_);
        if (fenced_ || !barrier_passed_ || publication_attempted_) {
            return Status(StatusCode::InvalidState, {});
        }
        publication_attempted_ = true;
    }
    try {
        auto checkpoint = checkpoint_prepared();
        if (!checkpoint.ok()) {
            return checkpoint.status();
        }
        if (!prepared_record_) {
            return Status(StatusCode::InvalidState, {});
        }
        auto record = *prepared_record_;
        auto boundary = sample_();
        if (!boundary.ok()) {
            return boundary.status();
        }
        for (const auto& prepared : prepared_) {
            if (prepared && boundary.value().monotonic_ns < prepared->prepared_monotonic) {
                return Status(StatusCode::InvalidState, {});
            }
        }
        record.run_operation->phase = RunPhase::OpenAuthorized;
        record.run_operation->authorization_boundary =
            RecoveryBoundary{boot_.kernel_uuid, boundary.value().monotonic_ns};
        const auto expected = record.record_revision++;
        auto published = recovery_->publish(record, expected);
        if (!published.ok()) {
            return published.status();
        }
        std::lock_guard<std::mutex> lock(opening_);
        if (fenced_) {
            return Status(StatusCode::Conflict, {});
        }
        opened_at_ = boundary.value().monotonic_ns;
        recording_base_ = recording_base;
        authorized_ = true;
        return {};
    } catch (const std::bad_alloc&) {
        (void)fence();
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> RunAuthorization::open(const TaskControlRecord& control) {
    std::lock_guard<std::mutex> lock(opening_);
    if (fenced_ || !authorized_ || !current(control)) {
        return Status(StatusCode::Conflict, {});
    }
    for (std::size_t i = 0; i < pipelines_.size(); ++i) {
        if (!prepared_[i]) {
            continue;
        }
        const auto& prepared = *prepared_[i];
        const pipeline::OpenAuthorization authorization{
            task_id_,
            prepared.pipeline_id,
            prepared.instance_id,
            generation_,
            pipelines_[i].effective_config_id,
            prepared.prepared_monotonic,
            boot_.id,
            opened_at_,
            recording_base_};
        if (pipelines_[i].controller->open_admission(authorization) !=
            pipeline::GateOpenResult::Opened) {
            fenced_ = true;
            for (const auto& pipeline : pipelines_) {
                (void)pipeline.controller->establish_control_fence();
            }
            return Status(StatusCode::Conflict, {});
        }
    }
    return {};
}

Result<void> RunAuthorization::fence() noexcept {
    std::lock_guard<std::mutex> lock(opening_);
    fenced_ = true;
    for (const auto& pipeline : pipelines_) {
        (void)pipeline.controller->establish_control_fence();
    }
    return {};
}

}  // namespace rodaq::task
