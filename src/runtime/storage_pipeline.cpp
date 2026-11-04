#include "runtime/storage_pipeline.hpp"

#include <rodaq/plugin/abi/storage.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace rodaq::runtime {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kOperationIndexMask = (std::uint64_t{1} << 32U) - 1U;
constexpr unsigned kOperationGenerationShift = 32U;

struct SubmitCallFrame {
    StoragePipeline* pipeline = nullptr;
    rodaq_storage_operation operation{};
};

thread_local const SubmitCallFrame* active_submit_frame = nullptr;

bool is_terminal(rodaq_storage_persistence_state state) noexcept {
    return state == RODAQ_STORAGE_PERSISTENCE_SUCCEEDED ||
           state == RODAQ_STORAGE_PERSISTENCE_FAILED || state == RODAQ_STORAGE_PERSISTENCE_UNKNOWN;
}

rodaq_storage_operation encode_operation(std::size_t index, std::uint32_t generation) noexcept {
    return rodaq_storage_operation{
        (static_cast<std::uint64_t>(generation) << kOperationGenerationShift) |
        (static_cast<std::uint64_t>(index) + 1U)};
}

}  // namespace

struct StoragePipeline::Slot {
    enum class DispatchState : std::uint8_t {
        Free,
        Reserved,
        Ready,
        InFlight,
    };

    DispatchState dispatch_state = DispatchState::Free;
    std::optional<RuntimeRecord> record;
    Reservation payload_reservation;
    rodaq_storage_operation operation{};
    std::uint32_t generation = 1;
    bool operation_active = false;
    bool handoff_accepted = false;
    SubmitReturn submit_return = SubmitReturn::NotReturned;
    PayloadAxis payload_axis = PayloadAxis::AwaitingReturn;
    PersistenceAxis persistence_axis = PersistenceAxis::AwaitingReturn;
    bool payload_event_seen = false;
    bool persistence_event_seen = false;
    rodaq_storage_persistence_state persistence_event_state = RODAQ_STORAGE_PERSISTENCE_PENDING;
    bool persistence_diagnostic_present = false;
    rodaq_error persistence_diagnostic{};
    bool scheduled = false;
    std::size_t payload_size = 0;
};

PipelineReservation::PipelineReservation(
    StoragePipeline& pipeline, std::size_t index, rodaq_storage_operation operation,
    Reservation record_reservation, Reservation payload_reservation) noexcept
: pipeline_(&pipeline),
  index_(index),
  operation_(operation),
  record_reservation_(std::move(record_reservation)),
  payload_reservation_(std::move(payload_reservation)) {}

PipelineReservation::PipelineReservation(PipelineReservation&& other) noexcept
: pipeline_(other.pipeline_),
  index_(other.index_),
  operation_(other.operation_),
  record_reservation_(std::move(other.record_reservation_)),
  payload_reservation_(std::move(other.payload_reservation_)) {
    other.pipeline_ = nullptr;
    other.index_ = 0;
    other.operation_ = {};
}

PipelineReservation& PipelineReservation::operator=(PipelineReservation&& other) noexcept {
    if (this != &other) {
        Cancel();
        pipeline_ = other.pipeline_;
        index_ = other.index_;
        operation_ = other.operation_;
        record_reservation_ = std::move(other.record_reservation_);
        payload_reservation_ = std::move(other.payload_reservation_);
        other.pipeline_ = nullptr;
        other.index_ = 0;
        other.operation_ = {};
    }
    return *this;
}

PipelineReservation::~PipelineReservation() { Cancel(); }

Reservation PipelineReservation::TakeRecordReservation() noexcept {
    return std::move(record_reservation_);
}

Reservation PipelineReservation::TakePayloadReservation() noexcept {
    return std::move(payload_reservation_);
}

void PipelineReservation::Cancel() noexcept {
    if (pipeline_ != nullptr) {
        StoragePipeline* pipeline = pipeline_;
        const std::size_t index = index_;
        pipeline_ = nullptr;
        record_reservation_.Release();
        payload_reservation_.Release();
        {
            std::lock_guard<std::mutex> lock(pipeline->mutex_);
            pipeline->ReturnIndexLocked(index);
            pipeline->resource_condition_.notify_all();
        }
    }
}

StoragePipeline::StoragePipeline(
    std::size_t capacity, std::size_t completion_capacity, std::size_t maximum_payload_bytes,
    std::size_t maximum_payload_inflight_bytes, plugin::StoragePluginInstance* storage,
    std::chrono::nanoseconds progress_timeout, FailureSink failure_sink,
    PayloadReleaseSink payload_release_sink, OperationCompleteSink operation_complete_sink,
    SubmissionBuilder submission_builder)
: capacity_(capacity),
  completion_capacity_(completion_capacity),
  maximum_payload_bytes_(maximum_payload_bytes),
  progress_timeout_(progress_timeout),
  storage_(storage),
  failure_sink_(std::move(failure_sink)),
  payload_release_sink_(std::move(payload_release_sink)),
  operation_complete_sink_(std::move(operation_complete_sink)),
  submission_builder_(std::move(submission_builder)),
  record_budget_(ResourceAmount{static_cast<std::uint64_t>(capacity), 0}),
  payload_budget_(ResourceAmount{0, static_cast<std::uint64_t>(maximum_payload_inflight_bytes)}),
  slots_(capacity),
  free_indices_(),
  dispatch_ring_(capacity),
  completion_ring_(completion_capacity) {
    if (capacity == 0 || completion_capacity == 0 || completion_capacity > capacity ||
        maximum_payload_bytes == 0 || maximum_payload_inflight_bytes == 0 || storage == nullptr ||
        !failure_sink_ || !payload_release_sink_ || !operation_complete_sink_ ||
        !submission_builder_) {
        throw std::invalid_argument("StoragePipeline configuration is invalid");
    }
    if (capacity > kOperationIndexMask) {
        throw std::invalid_argument("StoragePipeline capacity exceeds operation handle domain");
    }
    free_indices_.reserve(capacity);
    for (std::size_t offset = 0; offset < capacity; ++offset) {
        free_indices_.push_back(capacity - offset - 1U);
    }
}

StoragePipeline::~StoragePipeline() {
    stopping_ = true;
    CloseAdmission();
    CloseDispatch();
    completion_condition_.notify_all();
    submit_return_condition_.notify_all();
    watchdog_condition_.notify_all();
    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }
    if (completion_thread_.joinable()) {
        completion_thread_.join();
    }
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
}

Result<void> StoragePipeline::Start() {
    if (started_) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidState, "runtime", 0, "StoragePipeline is already started"));
    }
    try {
        watchdog_thread_ = std::thread(&StoragePipeline::WatchdogLoop, this);
        completion_thread_ = std::thread(&StoragePipeline::CompletionLoop, this);
        dispatch_thread_ = std::thread(&StoragePipeline::DispatchLoop, this);
        started_ = true;
        return {};
    } catch (const std::exception& error) {
        stopping_ = true;
        CloseDispatch();
        completion_condition_.notify_all();
        watchdog_condition_.notify_all();
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
        if (completion_thread_.joinable()) {
            completion_thread_.join();
        }
        if (watchdog_thread_.joinable()) {
            watchdog_thread_.join();
        }
        return nonstd::make_unexpected(Error(
            ErrorCode::Unavailable, "runtime", 0,
            std::string("Failed to start Storage pipeline workers: ") + error.what()));
    }
}

void StoragePipeline::OpenAdmission() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    admission_open_ = true;
    resource_condition_.notify_all();
}

void StoragePipeline::CloseAdmission() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    admission_open_ = false;
    resource_condition_.notify_all();
}

bool StoragePipeline::AdmissionOpen() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return admission_open_;
}

Result<PreGateAcquireResult> StoragePipeline::AcquirePreGate(
    std::size_t payload_bytes, AdmissionMode mode, std::chrono::steady_clock::time_point deadline) {
    if (payload_bytes > maximum_payload_bytes_) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "runtime", 0, "Payload exceeds runtime limit"));
    }

    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        if (!admission_open_ || stopping_) {
            return PreGateAcquireResult{PreGateAcquireOutcome::GateClosed, PipelineReservation()};
        }

        std::size_t index = kInvalidIndex;
        rodaq_storage_operation operation{};
        if (TryAcquireLocked(payload_bytes, index, operation)) {
            auto record_reservation = record_budget_.TryReserve(ResourceAmount{1, 0});
            auto payload_reservation = payload_budget_.TryReserve(ResourceAmount{0, payload_bytes});
            if (!record_reservation.has_value() || !payload_reservation.has_value()) {
                if (mode == AdmissionMode::Reject) {
                    return PreGateAcquireResult{
                        PreGateAcquireOutcome::ResourceUnavailable, PipelineReservation()};
                }
                if (Clock::now() >= deadline) {
                    return PreGateAcquireResult{
                        PreGateAcquireOutcome::Timeout, PipelineReservation()};
                }
                resource_condition_.wait_until(lock, deadline);
                continue;
            }
            free_indices_.pop_back();
            Slot& slot = slots_[index];
            slot.dispatch_state = Slot::DispatchState::Reserved;
            slot.operation = operation;
            slot.operation_active = true;
            slot.handoff_accepted = false;
            slot.submit_return = SubmitReturn::NotReturned;
            slot.payload_axis = PayloadAxis::AwaitingReturn;
            slot.persistence_axis = PersistenceAxis::AwaitingReturn;
            slot.payload_event_seen = false;
            slot.persistence_event_seen = false;
            slot.persistence_diagnostic_present = false;
            slot.scheduled = false;
            slot.payload_size = payload_bytes;
            return PreGateAcquireResult{
                PreGateAcquireOutcome::Acquired,
                PipelineReservation(
                    *this, index, operation, std::move(record_reservation.value()),
                    std::move(payload_reservation.value()))};
        }

        if (mode == AdmissionMode::Reject) {
            return PreGateAcquireResult{
                PreGateAcquireOutcome::ResourceUnavailable, PipelineReservation()};
        }
        if (Clock::now() >= deadline) {
            return PreGateAcquireResult{PreGateAcquireOutcome::Timeout, PipelineReservation()};
        }
        resource_condition_.wait_until(lock, deadline);
    }
}

bool StoragePipeline::TryAcquireLocked(
    std::size_t payload_bytes, std::size_t& index, rodaq_storage_operation& operation) {
    if (free_indices_.empty()) {
        return false;
    }
    if (record_budget_.Available().records == 0 ||
        payload_budget_.Available().bytes < payload_bytes) {
        return false;
    }

    index = free_indices_.back();
    Slot& slot = slots_[index];
    operation = encode_operation(index, slot.generation);
    return true;
}

void StoragePipeline::Publish(PipelineReservation&& reservation, RuntimeRecord&& record) noexcept {
    StoragePipeline* pipeline = reservation.pipeline_;
    if (pipeline == nullptr || pipeline != this) {
        std::terminate();
    }
    const std::size_t index = reservation.index_;
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (slot.dispatch_state != Slot::DispatchState::Reserved) {
        std::terminate();
    }
    slot.record.emplace(std::move(record));
    slot.payload_reservation = std::move(reservation.payload_reservation_);
    slot.dispatch_state = Slot::DispatchState::Ready;
    const std::size_t tail = (dispatch_head_ + dispatch_count_) % capacity_;
    dispatch_ring_[tail] = index;
    ++dispatch_count_;
    reservation.pipeline_ = nullptr;
    reservation.record_reservation_.Release();
    dispatch_condition_.notify_one();
}

void StoragePipeline::CloseDispatch() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatch_closed_ = true;
    dispatch_condition_.notify_all();
}

void StoragePipeline::CloseCallbacks() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_closed_ = true;
    stopping_ = true;
    completion_condition_.notify_all();
    watchdog_condition_.notify_all();
}

void StoragePipeline::JoinDispatch() noexcept {
    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }
}

void StoragePipeline::JoinCompletion() noexcept {
    if (completion_thread_.joinable()) {
        completion_thread_.join();
    }
}

bool StoragePipeline::WaitDispatchIdle(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return dispatch_condition_.wait_until(
        lock, deadline, [this] { return dispatch_count_ == 0 && !dispatch_call_active_; });
}

bool StoragePipeline::WaitOperationsIdle(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return resource_condition_.wait_until(lock, deadline, [this] {
        return std::none_of(
            slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.operation_active; });
    });
}

bool StoragePipeline::WaitStorageQuiescent(std::chrono::steady_clock::time_point deadline) {
    return WaitDispatchIdle(deadline) && WaitOperationsIdle(deadline);
}

bool StoragePipeline::HasUnresolvedOperations() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(
        slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.operation_active; });
}

std::size_t StoragePipeline::ActiveOperationCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.operation_active; }));
}

std::size_t StoragePipeline::ReadyDispatchCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatch_count_;
}

void StoragePipeline::DispatchLoop() {
    while (true) {
        std::size_t index = kInvalidIndex;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            dispatch_condition_.wait(
                lock, [this] { return dispatch_count_ != 0 || dispatch_closed_; });
            if (dispatch_count_ == 0) {
                return;
            }
            index = dispatch_ring_[dispatch_head_];
            dispatch_head_ = (dispatch_head_ + 1U) % capacity_;
            --dispatch_count_;
            slots_[index].dispatch_state = Slot::DispatchState::InFlight;
            dispatch_call_active_ = true;
            dispatch_overdue_reported_ = false;
            dispatch_deadline_ = Clock::now() + progress_timeout_;
            dispatch_condition_.notify_all();
            watchdog_condition_.notify_one();
        }
        DispatchOne(index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dispatch_call_active_ = false;
            dispatch_condition_.notify_all();
            watchdog_condition_.notify_all();
        }
    }
}

void StoragePipeline::WatchdogLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        watchdog_condition_.wait(lock, [this] {
            return dispatch_call_active_ || (stopping_ && !dispatch_call_active_);
        });
        if (!dispatch_call_active_) {
            return;
        }

        const auto deadline = dispatch_deadline_;
        if (!watchdog_condition_.wait_until(
                lock, deadline, [this] { return !dispatch_call_active_; })) {
            if (!dispatch_overdue_reported_) {
                dispatch_overdue_reported_ = true;
                lock.unlock();
                Fail(Error(
                    ErrorCode::Timeout, "storage", 0,
                    "Storage submit exceeded its progress deadline"));
                lock.lock();
                watchdog_condition_.wait(lock, [this] { return !dispatch_call_active_; });
            }
        }
    }
}

void StoragePipeline::DispatchOne(std::size_t index) {
    std::array<rodaq_timestamp_value, TimestampState::kMaxSlots> timestamp_values{};
    rodaq_storage_operation operation{};
    Result<rodaq_storage_submission> submission = nonstd::make_unexpected(
        Error(ErrorCode::InvalidState, "storage", 0, "Storage dispatch slot is unavailable"));
    const RuntimeRecord* record = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[index];
        if (!slot.operation_active || !slot.record.has_value()) {
            return;
        }
        operation = slot.operation;
        record = &slot.record.value();
    }
    try {
        submission = submission_builder_(*record, operation, timestamp_values);
    } catch (const std::exception& error) {
        submission =
            nonstd::make_unexpected(Error(ErrorCode::InternalError, "storage", 0, error.what()));
    } catch (...) {
        submission = nonstd::make_unexpected(
            Error(ErrorCode::InternalError, "storage", 0, "Storage submission construction threw"));
    }
    if (!submission.has_value()) {
        CompleteSubmit(index, nonstd::make_unexpected(submission.error()));
        Fail(Error(
            ErrorCode::InternalError, "storage", 0, "Storage submission construction failed"));
        return;
    }

    SubmitCallFrame frame{this, operation};
    const SubmitCallFrame* previous = active_submit_frame;
    active_submit_frame = &frame;
    Result<rodaq_storage_submit_result> result;
    try {
        result = storage_->submit(submission.value());
    } catch (const std::exception& error) {
        active_submit_frame = previous;
        result =
            nonstd::make_unexpected(Error(ErrorCode::InternalError, "storage", 0, error.what()));
        CompleteSubmit(index, std::move(result));
        return;
    } catch (...) {
        active_submit_frame = previous;
        result = nonstd::make_unexpected(
            Error(ErrorCode::InternalError, "storage", 0, "Storage submit threw"));
        CompleteSubmit(index, std::move(result));
        return;
    }
    active_submit_frame = previous;
    CompleteSubmit(index, std::move(result));
}

void StoragePipeline::CompleteSubmit(
    std::size_t index, Result<rodaq_storage_submit_result> result) {
    CompletionEffects effects;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[index];
        if (!slot.operation_active || !slot.record.has_value()) {
            return;
        }

        if (!result.has_value()) {
            slot.submit_return = SubmitReturn::ReturnedError;
            if (slot.payload_event_seen || slot.persistence_event_seen) {
                ApplyViolation(
                    effects, "Storage emitted completion before returning a non-OK submit result");
            }
            effects.failure.emplace(std::move(result.error()));
            slot.payload_axis = PayloadAxis::Consumed;
            slot.persistence_axis = PersistenceAxis::Failed;
            ReleasePayloadLocked(slot, effects);
            TerminalizeOperationLocked(index, slot, effects);
            submit_return_condition_.notify_all();
            resource_condition_.notify_all();
        } else {
            const rodaq_storage_submit_result& value = result.value();
            if ((value.payload_state != RODAQ_STORAGE_PAYLOAD_PENDING &&
                 value.payload_state != RODAQ_STORAGE_PAYLOAD_CONSUMED) ||
                (!is_terminal(value.persistence_state) &&
                 value.persistence_state != RODAQ_STORAGE_PERSISTENCE_PENDING)) {
                ApplyViolation(effects, "Storage returned an invalid submit state");
            }

            slot.submit_return = SubmitReturn::ReturnedOK;
            slot.handoff_accepted = true;
            if (value.payload_state == RODAQ_STORAGE_PAYLOAD_CONSUMED) {
                slot.payload_axis = PayloadAxis::Consumed;
                if (slot.payload_event_seen) {
                    ApplyViolation(
                        effects,
                        "Storage emitted PayloadConsumed for a synchronously consumed axis");
                }
                ReleasePayloadLocked(slot, effects);
            } else {
                slot.payload_axis = PayloadAxis::Pending;
            }

            switch (value.persistence_state) {
                case RODAQ_STORAGE_PERSISTENCE_SUCCEEDED:
                    slot.persistence_axis = PersistenceAxis::Succeeded;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_FAILED:
                    slot.persistence_axis = PersistenceAxis::Failed;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_UNKNOWN:
                    slot.persistence_axis = PersistenceAxis::Unknown;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_PENDING:
                    slot.persistence_axis = PersistenceAxis::Pending;
                    break;
                default:
                    slot.persistence_axis = PersistenceAxis::Unknown;
                    ApplyViolation(effects, "Storage returned an invalid persistence state");
                    break;
            }
            if (slot.persistence_axis != PersistenceAxis::Pending && slot.persistence_event_seen) {
                ApplyViolation(
                    effects, "Storage completed a synchronous-terminal persistence axis");
            }
            if (slot.payload_axis == PayloadAxis::Consumed &&
                slot.persistence_axis != PersistenceAxis::Pending) {
                TerminalizeOperationLocked(index, slot, effects);
            }
            submit_return_condition_.notify_all();
            resource_condition_.notify_all();
        }
    }

    NotifyPayloadReleased(effects);
    NotifyOperationCompleted(effects);
    if (effects.violation.has_value()) {
        Fail(std::move(effects.violation.value()));
    } else if (effects.failure.has_value()) {
        Fail(std::move(effects.failure.value()));
    }
    effects.payload_to_release = Payload();
    effects.record_to_destroy.reset();
}

void StoragePipeline::payload_consumed(rodaq_storage_operation operation) noexcept {
    std::optional<Error> violation;
    try {
        if (active_submit_frame != nullptr && active_submit_frame->pipeline == this &&
            active_submit_frame->operation.value == operation.value) {
            violation.emplace(
                ErrorCode::InternalError, "storage", 0,
                "Storage emitted same-stack PayloadConsumed during submit");
            Fail(std::move(violation.value()));
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t index = 0;
        if (!FindOperationLocked(operation, index)) {
            violation.emplace(
                ErrorCode::InternalError, "storage", 0,
                "Storage emitted PayloadConsumed for an unknown operation");
        } else if (callbacks_closed_) {
            violation.emplace(
                ErrorCode::InternalError, "storage", 0,
                "Storage emitted PayloadConsumed after callback quiescence");
        } else {
            Slot& slot = slots_[index];
            if (slot.payload_event_seen || slot.payload_axis == PayloadAxis::Consumed) {
                violation.emplace(
                    ErrorCode::InternalError, "storage", 0,
                    "Storage emitted duplicate PayloadConsumed");
            } else {
                slot.payload_event_seen = true;
                if (!ScheduleCompletionLocked(index)) {
                    violation.emplace(
                        ErrorCode::InternalError, "storage", 0,
                        "Completion scheduling capacity was exceeded");
                }
            }
        }
    } catch (...) {
        violation.emplace(
            ErrorCode::InternalError, "storage", 0, "Storage PayloadConsumed staging failed");
    }
    if (violation.has_value()) {
        Fail(std::move(violation.value()));
    }
}

void StoragePipeline::persistence_completed(
    rodaq_storage_operation operation, rodaq_storage_persistence_state state,
    const rodaq_error* error) noexcept {
    std::optional<Error> violation;
    try {
        if (active_submit_frame != nullptr && active_submit_frame->pipeline == this &&
            active_submit_frame->operation.value == operation.value) {
            violation.emplace(
                ErrorCode::InternalError, "storage", 0,
                "Storage emitted same-stack persistence completion during submit");
            Fail(std::move(violation.value()));
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t index = 0;
        if (!FindOperationLocked(operation, index)) {
            violation.emplace(
                ErrorCode::InternalError, "storage", 0,
                "Storage completed an unknown persistence operation");
        } else if (callbacks_closed_) {
            violation.emplace(
                ErrorCode::InternalError, "storage", 0,
                "Storage completed persistence after callback quiescence");
        } else if (!is_terminal(state)) {
            violation.emplace(
                ErrorCode::InternalError, "storage", 0,
                "Storage completed persistence with an invalid state");
        } else {
            Slot& slot = slots_[index];
            if (slot.persistence_event_seen ||
                slot.persistence_axis == PersistenceAxis::Succeeded ||
                slot.persistence_axis == PersistenceAxis::Failed ||
                slot.persistence_axis == PersistenceAxis::Unknown) {
                violation.emplace(
                    ErrorCode::InternalError, "storage", 0,
                    "Storage emitted duplicate persistence completion");
            } else if (state == RODAQ_STORAGE_PERSISTENCE_SUCCEEDED && error != nullptr) {
                violation.emplace(
                    ErrorCode::InternalError, "storage", 0,
                    "Storage supplied a diagnostic for successful persistence");
            } else if (state != RODAQ_STORAGE_PERSISTENCE_SUCCEEDED && error == nullptr) {
                violation.emplace(
                    ErrorCode::InternalError, "storage", 0,
                    "Storage persistence failure diagnostic is missing");
            } else {
                slot.persistence_event_seen = true;
                slot.persistence_event_state = state;
                CopyDiagnostic(error, slot);
                if (!ScheduleCompletionLocked(index)) {
                    violation.emplace(
                        ErrorCode::InternalError, "storage", 0,
                        "Completion scheduling capacity was exceeded");
                }
            }
        }
    } catch (...) {
        violation.emplace(
            ErrorCode::InternalError, "storage", 0,
            "Storage persistence completion staging failed");
    }
    if (violation.has_value()) {
        Fail(std::move(violation.value()));
    }
}

void StoragePipeline::CompletionLoop() {
    while (true) {
        std::size_t index = kInvalidIndex;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            completion_condition_.wait(
                lock, [this] { return completion_count_ != 0 || stopping_; });
            if (completion_count_ == 0) {
                return;
            }
            index = completion_ring_[completion_head_];
            completion_head_ = (completion_head_ + 1U) % completion_capacity_;
            --completion_count_;
        }

        if (!WaitForSubmitReturn(index, Clock::now() + progress_timeout_)) {
            Fail(Error(
                ErrorCode::Timeout, "storage", 0,
                "Storage completion arrived but submit return was not reconciled"));
            continue;
        }
        CompletionEffects effects = ProcessCompletion(index);
        NotifyPayloadReleased(effects);
        NotifyOperationCompleted(effects);
        if (effects.violation.has_value()) {
            Fail(std::move(effects.violation.value()));
        }
        effects.payload_to_release = Payload();
        effects.record_to_destroy.reset();
    }
}

bool StoragePipeline::WaitForSubmitReturn(
    std::size_t index, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return submit_return_condition_.wait_until(lock, deadline, [this, index] {
        const Slot& slot = slots_[index];
        return !slot.operation_active || slot.submit_return != SubmitReturn::NotReturned ||
               stopping_;
    });
}

StoragePipeline::CompletionEffects StoragePipeline::ProcessCompletion(std::size_t index) {
    CompletionEffects effects;
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (!slot.operation_active || slot.submit_return == SubmitReturn::NotReturned) {
        return effects;
    }

    if (slot.payload_event_seen) {
        slot.payload_event_seen = false;
        if (slot.payload_axis == PayloadAxis::Consumed) {
            ApplyViolation(effects, "PayloadConsumed callback was already terminal");
        } else {
            slot.payload_axis = PayloadAxis::Consumed;
            ReleasePayloadLocked(slot, effects);
        }
    }

    if (slot.persistence_event_seen) {
        slot.persistence_event_seen = false;
        if (slot.persistence_axis == PersistenceAxis::Succeeded ||
            slot.persistence_axis == PersistenceAxis::Failed ||
            slot.persistence_axis == PersistenceAxis::Unknown) {
            ApplyViolation(effects, "Persistence callback was already terminal");
        } else {
            switch (slot.persistence_event_state) {
                case RODAQ_STORAGE_PERSISTENCE_SUCCEEDED:
                    slot.persistence_axis = PersistenceAxis::Succeeded;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_FAILED:
                    slot.persistence_axis = PersistenceAxis::Failed;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_UNKNOWN:
                    slot.persistence_axis = PersistenceAxis::Unknown;
                    break;
                default:
                    ApplyViolation(effects, "Invalid staged persistence state");
                    break;
            }
        }
    }

    if (slot.payload_axis == PayloadAxis::Consumed &&
        slot.persistence_axis != PersistenceAxis::Pending &&
        slot.persistence_axis != PersistenceAxis::AwaitingReturn) {
        TerminalizeOperationLocked(index, slot, effects);
    } else {
        slot.scheduled = false;
    }
    resource_condition_.notify_all();
    completion_condition_.notify_all();
    return effects;
}

void StoragePipeline::ReleasePayloadLocked(Slot& slot, CompletionEffects& effects) noexcept {
    if (slot.payload_axis != PayloadAxis::Consumed || !slot.record.has_value()) {
        return;
    }
    effects.payload_to_release = slot.record->TakePayload();
    slot.payload_reservation.Release();
    effects.payload_released = true;
    effects.released_bytes = slot.payload_size;
    resource_condition_.notify_all();
}

void StoragePipeline::TerminalizeOperationLocked(
    std::size_t index, Slot& slot, CompletionEffects& effects) noexcept {
    if (!slot.operation_active) {
        return;
    }
    if (slot.record.has_value()) {
        effects.record_to_destroy.emplace(std::move(slot.record.value()));
        slot.record.reset();
    }
    effects.operation_completed = true;
    slot.payload_size = 0;
    slot.operation_active = false;
    slot.dispatch_state = Slot::DispatchState::Free;
    slot.scheduled = false;
    ReturnIndexLocked(index);
}

void StoragePipeline::NotifyPayloadReleased(const CompletionEffects& effects) noexcept {
    if (!effects.payload_released) {
        return;
    }
    try {
        payload_release_sink_(effects.released_bytes);
    } catch (...) {
        Fail(Error(
            ErrorCode::InternalError, "storage", 0,
            "Payload-release observer raised an exception"));
    }
}

void StoragePipeline::NotifyOperationCompleted(const CompletionEffects& effects) noexcept {
    if (!effects.operation_completed) {
        return;
    }
    try {
        operation_complete_sink_();
    } catch (...) {
        Fail(Error(
            ErrorCode::InternalError, "storage", 0,
            "Storage-completion observer raised an exception"));
    }
}

void StoragePipeline::ReturnIndexLocked(std::size_t index) noexcept {
    Slot& slot = slots_[index];
    if (slot.dispatch_state == Slot::DispatchState::Free &&
        std::find(free_indices_.begin(), free_indices_.end(), index) != free_indices_.end()) {
        return;
    }
    slot.dispatch_state = Slot::DispatchState::Free;
    slot.operation_active = false;
    if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
        return;
    }
    ++slot.generation;
    free_indices_.push_back(index);
    resource_condition_.notify_all();
}

bool StoragePipeline::FindOperationLocked(
    rodaq_storage_operation operation, std::size_t& index) noexcept {
    if (operation.value == 0) {
        return false;
    }
    const std::uint64_t raw_index = (operation.value & kOperationIndexMask) - 1U;
    if (raw_index >= slots_.size()) {
        return false;
    }
    const std::size_t candidate = static_cast<std::size_t>(raw_index);
    const Slot& slot = slots_[candidate];
    if (!slot.operation_active ||
        slot.generation !=
            static_cast<std::uint32_t>(operation.value >> kOperationGenerationShift)) {
        return false;
    }
    index = candidate;
    return true;
}

bool StoragePipeline::ScheduleCompletionLocked(std::size_t index) noexcept {
    Slot& slot = slots_[index];
    if (slot.scheduled) {
        return true;
    }
    if (completion_count_ >= completion_capacity_) {
        return false;
    }
    const std::size_t tail = (completion_head_ + completion_count_) % completion_capacity_;
    completion_ring_[tail] = index;
    ++completion_count_;
    slot.scheduled = true;
    completion_condition_.notify_one();
    return true;
}

void StoragePipeline::ApplyViolation(CompletionEffects& effects, const char* message) noexcept {
    if (!effects.violation.has_value()) {
        try {
            effects.violation.emplace(ErrorCode::InternalError, "storage", 0, message);
        } catch (...) {
        }
    }
}

void StoragePipeline::Fail(Error error) noexcept {
    if (failure_reported_.exchange(true)) {
        return;
    }
    try {
        failure_sink_(std::move(error));
    } catch (...) {
    }
}

void StoragePipeline::CopyDiagnostic(const rodaq_error* error, Slot& slot) noexcept {
    slot.persistence_diagnostic_present = error != nullptr;
    if (error != nullptr) {
        slot.persistence_diagnostic = *error;
    } else {
        slot.persistence_diagnostic = {};
    }
}

}  // namespace rodaq::runtime
