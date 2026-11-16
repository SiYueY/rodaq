#include "task_controller.hpp"

#include "identity.hpp"
#include "config.hpp"
#include "../work_id.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq::task {
namespace {

const StoredOperation* current_operation(const TaskControlRecord& record) noexcept {
    if (!record.current_operation_id) {
        return nullptr;
    }
    const auto found = std::find_if(
        record.operation_history.entries.begin(), record.operation_history.entries.end(),
        [&](const auto& entry) {
            return entry.snapshot.operation_id == *record.current_operation_id;
        });
    return found == record.operation_history.entries.end() ? nullptr : &*found;
}

bool lifecycle(OperationKind kind) noexcept {
    return kind == OperationKind::Start || kind == OperationKind::Pause ||
           kind == OperationKind::Resume || kind == OperationKind::Stop;
}

bool closing(OperationKind kind) noexcept {
    return kind == OperationKind::Pause || kind == OperationKind::Stop;
}

bool transition(DesiredState desired, OperationKind kind) noexcept {
    switch (kind) {
        case OperationKind::Start:
            return desired == DesiredState::Stopped;
        case OperationKind::Pause:
            return desired == DesiredState::Running;
        case OperationKind::Resume:
            return desired == DesiredState::Paused;
        case OperationKind::Stop:
            return desired == DesiredState::Running || desired == DesiredState::Paused;
        default:
            return false;
    }
}

DesiredState target(OperationKind kind) noexcept {
    if (kind == OperationKind::Pause) {
        return DesiredState::Paused;
    }
    return (kind == OperationKind::Stop || kind == OperationKind::Configure)
               ? DesiredState::Stopped
               : DesiredState::Running;
}

}  // namespace

TaskController::TaskController(
    std::shared_ptr<const TaskControlRecord> durable, HistoryPolicy policy) noexcept
: durable_(std::move(durable)), policy_(policy) {}

Result<std::unique_ptr<TaskController>> TaskController::create(
    TaskControlRecord durable, HistoryPolicy policy) {
    auto valid = validate_control_record(durable, policy);
    if (!valid.ok()) {
        return valid.status();
    }
    try {
        return std::unique_ptr<TaskController>(new TaskController(
            std::make_shared<const TaskControlRecord>(std::move(durable)), policy));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

const StoredOperation* TaskController::pending_request(std::string_view key) const noexcept {
    if (stop_ && stop_->operation.request.request_key == key) {
        return &stop_->operation;
    }
    if (flight_ && flight_->publication == ControlPublicationKind::Intent) {
        const auto* operation = current_operation(*flight_->candidate);
        if (operation && operation->request.request_key == key) {
            return operation;
        }
    }
    return nullptr;
}

Result<std::shared_ptr<const ControlWork>> TaskController::make_work(
    TaskControlRecord candidate, ControlPublicationKind publication,
    std::uint64_t expected_revision, std::shared_ptr<const ConfigPublication> config) const {
    auto valid = validate_control_record(candidate, policy_);
    if (!valid.ok()) {
        return valid.status();
    }
    auto id = next_work_id();
    if (!id.ok()) {
        return id.status();
    }
    try {
        auto record = std::make_shared<const TaskControlRecord>(std::move(candidate));
        return std::make_shared<const ControlWork>(ControlWork{
            id.value(), ControlWorkKind::Publish, publication, expected_revision, std::move(record),
            std::move(config)});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::shared_ptr<const ControlWork>> TaskController::make_intent(
    const TaskControlRecord& base, StoredOperation operation,
    std::shared_ptr<const ConfigPublication> config) const {
    if (base.control_generation == std::numeric_limits<std::uint64_t>::max() ||
        base.record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    try {
        auto candidate = base;
        ++candidate.control_generation;
        ++candidate.record_revision;
        operation.snapshot.control_generation = candidate.control_generation;
        if (candidate.current_operation_id) {
            for (auto& old : candidate.operation_history.entries) {
                if (old.snapshot.operation_id == *candidate.current_operation_id &&
                    !terminal(old.snapshot.state)) {
                    old.snapshot.state = OperationState::Superseded;
                    old.snapshot.terminal_at =
                        std::max(operation.snapshot.accepted_at, old.snapshot.accepted_at);
                }
            }
        }
        auto history = append_operation(
            candidate.operation_history, operation, candidate.current_operation_id, policy_,
            operation.snapshot.accepted_at);
        if (!history.ok()) {
            return history.status();
        }
        candidate.operation_history = std::move(history.value());
        candidate.current_operation_id = operation.snapshot.operation_id;
        candidate.desired_state = target(operation.snapshot.kind);
        if (config) {
            candidate.active_config_version = config->projection.config_version;
        }
        return make_work(
            std::move(candidate), ControlPublicationKind::Intent, base.record_revision,
            std::move(config));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ControlDecision> TaskController::request_lifecycle(
    OperationKind kind, const RequestIdentity& request, const ControlPrecondition& precondition,
    WallTime now, const LifecycleFacts& facts) {
    if (!lifecycle(kind)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto digest = lifecycle_request_digest(durable_->task_id, kind, precondition);
    if (!digest.ok()) {
        return digest.status();
    }
    auto known = find_request(durable_->operation_history, request, digest.value());
    if (known.ok()) {
        return ControlDecision{known.value(), true, false, {}};
    }
    if (known.status().code() != StatusCode::NotFound) {
        return known.status();
    }
    const auto* pending = pending_request(request.request_key);
    if (pending) {
        if (pending->request_digest != digest.value()) {
            return Status(StatusCode::Conflict, {});
        }
        return ControlDecision{pending->snapshot.operation_id, false, false, {}};
    }
    if (precondition.has_expected_control_generation &&
        precondition.expected_control_generation != durable_->control_generation) {
        return Status(StatusCode::Conflict, {});
    }
    if (!facts.execution_available ||
        ((kind == OperationKind::Start || kind == OperationKind::Resume) && !facts.quiescent) ||
        durable_->reset_intent || (protected_ && kind != OperationKind::Stop)) {
        return Status(StatusCode::InvalidState, {});
    }
    if ((flight_ || stop_) && kind != OperationKind::Stop) {
        return Status(StatusCode::Conflict, {});
    }
    if (stop_) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    // Stop may fence a Start whose candidate is not yet durable, including
    // while the durable Desired State still reads Stopped.
    const bool superseding_publish = kind == OperationKind::Stop && flight_;
    if (!transition(durable_->desired_state, kind) &&
        (!superseding_publish || !transition(flight_->candidate->desired_state, kind))) {
        return Status(StatusCode::InvalidState, {});
    }
    try {
        auto id = new_operation_id();
        if (!id.ok()) {
            return id.status();
        }
        StoredOperation operation;
        operation.request = request;
        operation.request_digest = std::move(digest.value());
        operation.snapshot.operation_id = id.value();
        operation.snapshot.kind = kind;
        operation.snapshot.accepted_at = now;
        if (superseding_publish) {
            stop_.emplace(PendingStop{std::move(operation)});
            opening_fenced_ = true;
            return ControlDecision{id.value(), false, true, {}};
        }
        auto work = make_intent(*durable_, std::move(operation));
        if (!work.ok()) {
            return work.status();
        }
        flight_ = work.value();
        actual_work_inflight_ = true;
        opening_fenced_ = opening_fenced_ || closing(kind);
        return ControlDecision{id.value(), false, closing(kind), flight_};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ControlDecision> TaskController::request_configure(
    const TaskConfig& config, const RequestIdentity& request,
    const ControlPrecondition& precondition, WallTime now, const ConfigureFacts& facts,
    const TaskConfigLimits& limits) {
    auto digest = configure_request_digest(durable_->task_id, config, precondition, limits);
    if (!digest.ok()) {
        return digest.status();
    }
    auto known = find_request(durable_->operation_history, request, digest.value());
    if (known.ok()) {
        return ControlDecision{known.value(), true, false, {}};
    }
    if (known.status().code() != StatusCode::NotFound) {
        return known.status();
    }
    const auto* pending = pending_request(request.request_key);
    if (pending) {
        if (pending->request_digest != digest.value()) {
            return Status(StatusCode::Conflict, {});
        }
        return ControlDecision{pending->snapshot.operation_id, false, false, {}};
    }
    if (precondition.has_expected_control_generation &&
        precondition.expected_control_generation != durable_->control_generation) {
        return Status(StatusCode::Conflict, {});
    }
    if (protected_ || durable_->reset_intent || durable_->desired_state != DesiredState::Stopped ||
        !facts.quiescent) {
        return Status(StatusCode::InvalidState, {});
    }
    if (flight_ || stop_) {
        return Status(StatusCode::Conflict, {});
    }
    if (!facts.deployment_allowed) {
        return Status(StatusCode::PermissionDenied, {});
    }
    auto version = config_version(facts.active_config, limits);
    if (!version.ok()) {
        return version.status();
    }
    if (version.value() != durable_->active_config_version ||
        facts.identities.record_revision == 0 ||
        facts.identities.observed_config_version != durable_->active_config_version ||
        !(facts.identities.task_id == durable_->task_id)) {
        return Status(StatusCode::DataLoss, {});
    }
    auto assigned = assign_config_change(
        facts.active_config, config, facts.identities.pipeline_id_high_water,
        facts.identities.stream_id_high_water, limits);
    if (!assigned.ok()) {
        return assigned.status();
    }
    auto projection = identity_projection(assigned.value(), limits);
    if (!projection.ok()) {
        return projection.status();
    }
    try {
        auto publication = std::make_shared<const ConfigPublication>(ConfigPublication{
            std::move(assigned.value()), std::move(projection.value()),
            facts.identities.record_revision});
        auto id = new_operation_id();
        if (!id.ok()) {
            return id.status();
        }
        StoredOperation operation;
        operation.request = request;
        operation.request_digest = std::move(digest.value());
        operation.snapshot.operation_id = id.value();
        operation.snapshot.kind = OperationKind::Configure;
        operation.snapshot.accepted_at = now;
        auto work = make_intent(*durable_, std::move(operation), std::move(publication));
        if (!work.ok()) {
            return work.status();
        }
        flight_ = std::move(work.value());
        actual_work_inflight_ = true;
        return ControlDecision{id.value(), false, false, flight_};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::shared_ptr<const ControlWork>> TaskController::update_operation(
    const OperationSnapshot& result) {
    const auto* operation = current_operation(*durable_);
    if (protected_ || flight_ || stop_ || !operation ||
        !(operation->snapshot.operation_id == result.operation_id) ||
        operation->snapshot.control_generation != result.control_generation ||
        operation->snapshot.kind != result.kind ||
        operation->snapshot.accepted_at != result.accepted_at ||
        terminal(operation->snapshot.state) || result.state == OperationState::Accepted ||
        (result.state != OperationState::InProgress && !terminal(result.state))) {
        return Status(StatusCode::Conflict, {});
    }
    if (durable_->record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    try {
        auto candidate = *durable_;
        ++candidate.record_revision;
        for (auto& entry : candidate.operation_history.entries) {
            if (entry.snapshot.operation_id == result.operation_id) {
                entry.snapshot = result;
            }
        }
        auto work = make_work(
            std::move(candidate), ControlPublicationKind::OperationUpdate,
            durable_->record_revision);
        if (!work.ok()) {
            return work.status();
        }
        flight_ = work.value();
        actual_work_inflight_ = true;
        return flight_;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::shared_ptr<const ControlWork>> TaskController::reconciliation_work() {
    if (!protected_ || !flight_ || actual_work_inflight_) {
        return Status(StatusCode::InvalidState, {});
    }
    auto id = next_work_id();
    if (!id.ok()) {
        return id.status();
    }
    try {
        auto frame = *flight_;
        frame.work_id = id.value();
        frame.kind = ControlWorkKind::Reconcile;
        auto work = std::make_shared<const ControlWork>(std::move(frame));
        flight_ = std::move(work);
        actual_work_inflight_ = true;
        return flight_;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ControlCompletion> TaskController::persistence_completed(
    std::uint64_t work_id, Result<TaskControlRecord> authority, Status failure) {
    ControlCompletion completion;
    completion.persistence_protected = protected_;
    if (!flight_ || !actual_work_inflight_ || flight_->work_id != work_id) {
        return completion;
    }
    try {
        const auto* operation = current_operation(*flight_->candidate);
        const auto protect = [&](Status error) -> ControlCompletion {
            completion.resolved = ControlResolution{
                operation->snapshot.operation_id, flight_->publication, false, error};
            if (stop_) {
                completion.stop_resolution = ControlResolution{
                    stop_->operation.snapshot.operation_id, ControlPublicationKind::Intent, false,
                    error};
            }
            unresolved_failure_ = std::move(error);
            protected_ = true;
            opening_fenced_ = true;
            actual_work_inflight_ = false;
            completion.recognized = true;
            completion.persistence_protected = true;
            return std::move(completion);
        };
        if (!authority.ok()) {
            return protect(authority.status());
        }
        auto actual = encode_control_record(authority.value(), policy_);
        if (!actual.ok() && actual.status().code() != StatusCode::ResourceExhausted) {
            return protect(
                Status(StatusCode::DataLoss, "Invalid Task Control completion authority"));
        }
        auto candidate = encode_control_record(*flight_->candidate, policy_);
        auto previous = encode_control_record(*durable_, policy_);
        if (!actual.ok() || !candidate.ok() || !previous.ok()) {
            return !actual.ok()      ? actual.status()
                   : !candidate.ok() ? candidate.status()
                                     : previous.status();
        }
        const bool committed = actual.value() == candidate.value();
        if (!committed &&
            (actual.value() != previous.value() || (failure.ok() && unresolved_failure_.ok()))) {
            return protect(
                Status(StatusCode::DataLoss, "Unrecognized Task Control completion authority"));
        }
        Status status;
        if (!committed) {
            status = failure.ok() ? unresolved_failure_ : std::move(failure);
        }
        completion.resolved = ControlResolution{
            operation->snapshot.operation_id, flight_->publication, committed, std::move(status)};
        // Prepare every result and successor before changing the cached durable
        // authority: allocation failure cannot lose an actual completion event.
        if (stop_) {
            auto next = make_intent(committed ? *flight_->candidate : *durable_, stop_->operation);
            if (next.ok()) {
                completion.next_work = std::move(next.value());
            } else {
                completion.stop_resolution = ControlResolution{
                    stop_->operation.snapshot.operation_id, ControlPublicationKind::Intent, false,
                    next.status()};
            }
        }
        const bool enable_run = committed &&
                                flight_->publication == ControlPublicationKind::Intent && !stop_ &&
                                (operation->snapshot.kind == OperationKind::Start ||
                                 operation->snapshot.kind == OperationKind::Resume);
        if (committed) {
            durable_ = flight_->candidate;
        }
        if (enable_run) {
            opening_fenced_ = false;
        }
        flight_ = completion.next_work;
        actual_work_inflight_ = static_cast<bool>(flight_);
        stop_.reset();
        protected_ = false;
        unresolved_failure_ = {};
        completion.recognized = true;
        completion.persistence_protected = false;
        return completion;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

bool TaskController::run_is_current(
    OperationId operation, ControlGeneration generation) const noexcept {
    const auto* current = current_operation(*durable_);
    return !protected_ && !opening_fenced_ && durable_->desired_state == DesiredState::Running &&
           current && current->snapshot.operation_id == operation &&
           current->snapshot.control_generation == generation &&
           (current->snapshot.kind == OperationKind::Start ||
            current->snapshot.kind == OperationKind::Resume) &&
           current->snapshot.state != OperationState::Failed &&
           current->snapshot.state != OperationState::Superseded;
}

}  // namespace rodaq::task
