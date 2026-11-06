#include "storage/storage_pipeline.hpp"

#include "plugin/plugin_quarantine.hpp"

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

namespace rodaq {

bool PipelineReservation::active() const noexcept { return pipeline_ != nullptr; }
std::size_t PipelineReservation::index() const noexcept { return index_; }
rodaq_storage_operation PipelineReservation::operation() const noexcept { return operation_; }

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
    DispatchState dispatch_state = DispatchState::Free;
    std::optional<AcceptedRecord> record;
    Payload release_payload;
    Reservation payload_reservation;
    rodaq_storage_operation operation{};
    std::uint32_t generation = 1;
    bool operation_active = false;
    bool handoff_accepted = false;
    SubmitReturn submit_return = SubmitReturn::NotReturned;
    PayloadAxis payload_axis = PayloadAxis::AwaitingReturn;
    PayloadReleaseState payload_release_state = PayloadReleaseState::NotRequired;
    PersistenceAxis persistence_axis = PersistenceAxis::AwaitingReturn;
    bool payload_event_seen = false;
    bool persistence_event_seen = false;
    rodaq_storage_persistence_state persistence_event_state = RODAQ_STORAGE_PERSISTENCE_PENDING;
    bool persistence_diagnostic_present = false;
    rodaq_status persistence_diagnostic{};
    bool scheduled = false;
    std::size_t payload_size = 0;
};

PipelineReservation::PipelineReservation(
    StoragePipeline& pipeline, std::size_t index, rodaq_storage_operation operation,
    Reservation record_reservation, Reservation payload_reservation,
    PayloadPoolReservation payload_pool_reservation) noexcept
: pipeline_(&pipeline),
  index_(index),
  operation_(operation),
  record_reservation_(std::move(record_reservation)),
  payload_reservation_(std::move(payload_reservation)),
  payload_pool_reservation_(std::move(payload_pool_reservation)) {}

PipelineReservation::PipelineReservation(PipelineReservation&& other) noexcept
: pipeline_(other.pipeline_),
  index_(other.index_),
  operation_(other.operation_),
  record_reservation_(std::move(other.record_reservation_)),
  payload_reservation_(std::move(other.payload_reservation_)),
  payload_pool_reservation_(std::move(other.payload_pool_reservation_)) {
    other.pipeline_ = nullptr;
    other.index_ = 0;
    other.operation_ = {};
}

PipelineReservation& PipelineReservation::operator=(PipelineReservation&& other) noexcept {
    if (this != &other) {
        cancel();
        pipeline_ = other.pipeline_;
        index_ = other.index_;
        operation_ = other.operation_;
        record_reservation_ = std::move(other.record_reservation_);
        payload_reservation_ = std::move(other.payload_reservation_);
        payload_pool_reservation_ = std::move(other.payload_pool_reservation_);
        other.pipeline_ = nullptr;
        other.index_ = 0;
        other.operation_ = {};
    }
    return *this;
}

PipelineReservation::~PipelineReservation() { cancel(); }

Reservation PipelineReservation::take_record_reservation() noexcept {
    return std::move(record_reservation_);
}

Reservation PipelineReservation::take_payload_reservation() noexcept {
    return std::move(payload_reservation_);
}

PayloadPoolReservation PipelineReservation::take_payload_pool_reservation() noexcept {
    return std::move(payload_pool_reservation_);
}

void PipelineReservation::cancel() noexcept {
    if (pipeline_ != nullptr) {
        StoragePipeline* pipeline = pipeline_;
        const std::size_t index = index_;
        pipeline_ = nullptr;
        payload_pool_reservation_.release();
        record_reservation_.release();
        payload_reservation_.release();
        {
            std::lock_guard<std::mutex> lock(pipeline->mutex_);
            StoragePipeline::Slot& slot = pipeline->slots_[index];
            if (slot.dispatch_state == StoragePipeline::DispatchState::Reserved) {
                slot.dispatch_state = StoragePipeline::DispatchState::Terminalizing;
                pipeline->return_index_locked(index);
            }
            pipeline->resource_condition_.notify_all();
        }
    }
}

StoragePipeline::StoragePipeline(
    std::size_t capacity, std::size_t completion_capacity, std::size_t maximum_payload_bytes,
    std::size_t maximum_payload_inflight_bytes, plugin::StoragePluginInstance* storage,
    std::chrono::nanoseconds progress_timeout, std::size_t release_worker_count,
    FailureSink failure_sink, PayloadReleaseSink payload_release_sink,
    OperationCompleteSink operation_complete_sink, LatencySink latency_sink,
    SubmissionBuilder submission_builder)
: capacity_(capacity),
  completion_capacity_(completion_capacity),
  maximum_payload_bytes_(maximum_payload_bytes),
  progress_timeout_(progress_timeout),
  release_worker_count_(release_worker_count),
  storage_(storage),
  failure_sink_(std::move(failure_sink)),
  payload_release_sink_(std::move(payload_release_sink)),
  operation_complete_sink_(std::move(operation_complete_sink)),
  latency_sink_(std::move(latency_sink)),
  submission_builder_(std::move(submission_builder)),
  record_budget_(ResourceAmount{static_cast<std::uint64_t>(capacity), 0}),
  payload_budget_(ResourceAmount{0, static_cast<std::uint64_t>(maximum_payload_inflight_bytes)}),
  slots_(capacity),
  free_indices_(),
  dispatch_ring_(capacity),
  completion_ring_(completion_capacity),
  release_ring_(capacity) {
    if (capacity == 0 || completion_capacity == 0 || completion_capacity > capacity ||
        maximum_payload_bytes == 0 || maximum_payload_inflight_bytes == 0 || storage == nullptr ||
        release_worker_count == 0 || !failure_sink_ || !payload_release_sink_ ||
        !operation_complete_sink_ || !latency_sink_ || !submission_builder_) {
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
    close_admission();
    close_dispatch();
    close_release();
    completion_condition_.notify_all();
    submit_return_condition_.notify_all();
    watchdog_condition_.notify_all();
    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }
    if (completion_thread_.joinable()) {
        completion_thread_.join();
    }
    for (std::thread& worker : release_threads_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
}

Result<void> StoragePipeline::start() {
    if (started_) {
        return rodaq::Status(
            StatusCode::InvalidState, "runtime", 0, "StoragePipeline is already started");
    }
    try {
        watchdog_thread_ = std::thread([this] {
            watchdog_loop();
            watchdog_done_.store(true, std::memory_order_release);
        });
        completion_thread_ = std::thread([this] {
            completion_loop();
            completion_done_.store(true, std::memory_order_release);
        });
        dispatch_thread_ = std::thread([this] {
            dispatch_loop();
            dispatch_done_.store(true, std::memory_order_release);
        });
        release_threads_.reserve(release_worker_count_);
        release_worker_states_.reserve(release_worker_count_);
        for (std::size_t index = 0; index < release_worker_count_; ++index) {
            auto state = std::make_shared<ReleaseWorkerState>();
            release_worker_states_.push_back(state);
            release_threads_.emplace_back(&StoragePipeline::release_loop, this, state);
        }
        started_ = true;
        return {};
    } catch (const std::exception& error) {
        stopping_ = true;
        close_dispatch();
        close_release();
        completion_condition_.notify_all();
        watchdog_condition_.notify_all();
        if (dispatch_thread_.joinable()) {
            dispatch_thread_.join();
        }
        if (completion_thread_.joinable()) {
            completion_thread_.join();
        }
        for (std::thread& worker : release_threads_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (watchdog_thread_.joinable()) {
            watchdog_thread_.join();
        }
        return rodaq::Status(
            StatusCode::Unavailable, "runtime", 0,
            std::string("Failed to start Storage pipeline workers: ") + error.what());
    }
}

void StoragePipeline::open_admission() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_cutoff_.load(std::memory_order_acquire)) {
        return;
    }
    admission_open_ = true;
    resource_condition_.notify_all();
}

void StoragePipeline::close_admission() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    admission_open_ = false;
    resource_condition_.notify_all();
}

bool StoragePipeline::admission_open() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return admission_open_;
}

Result<PreGateAcquireResult> StoragePipeline::acquire_pre_gate(
    std::size_t payload_bytes, AdmissionMode mode, PayloadPool* copy_pool, bool copy_payload,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<std::uint64_t>& eligibility_generation) {
    if (payload_bytes > maximum_payload_bytes_) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "runtime", 0, "Payload exceeds runtime limit");
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t initial_eligibility_generation =
        eligibility_generation.load(std::memory_order_acquire);
    while (true) {
        if (!admission_open_ || stopping_) {
            return PreGateAcquireResult{PreGateAcquireOutcome::GateClosed, PipelineReservation()};
        }
        if (eligibility_generation.load(std::memory_order_acquire) !=
            initial_eligibility_generation) {
            return PreGateAcquireResult{
                PreGateAcquireOutcome::EligibilityChanged, PipelineReservation()};
        }

        std::size_t index = kInvalidIndex;
        rodaq_storage_operation operation{};
        if (try_acquire_locked(payload_bytes, index, operation)) {
            auto record_reservation = record_budget_.try_reserve(ResourceAmount{1, 0});
            auto payload_reservation =
                payload_budget_.try_reserve(ResourceAmount{0, payload_bytes});
            PayloadPoolReservation pool_reservation;
            bool pool_reserved = !copy_payload || payload_bytes == 0;
            if (record_reservation.ok() && payload_reservation.ok() && copy_payload &&
                payload_bytes != 0) {
                if (copy_pool == nullptr) {
                    return rodaq::Status(
                        StatusCode::InvalidState, "runtime", 0,
                        "Copy Payload requires a PayloadPool");
                }
                auto reserved = copy_pool->try_reserve(payload_bytes);
                if (reserved.ok()) {
                    pool_reservation = std::move(reserved.value());
                    pool_reserved = true;
                }
            }
            if (!record_reservation.ok() || !payload_reservation.ok() || !pool_reserved) {
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
            slot.dispatch_state = DispatchState::Reserved;
            slot.operation = operation;
            slot.operation_active = true;
            slot.handoff_accepted = false;
            slot.submit_return = SubmitReturn::NotReturned;
            slot.payload_axis = PayloadAxis::AwaitingReturn;
            slot.payload_release_state = PayloadReleaseState::NotRequired;
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
                    std::move(payload_reservation.value()), std::move(pool_reservation))};
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

bool StoragePipeline::try_acquire_locked(
    std::size_t payload_bytes, std::size_t& index, rodaq_storage_operation& operation) {
    if (free_indices_.empty()) {
        return false;
    }
    if (record_budget_.available().records == 0 ||
        payload_budget_.available().bytes < payload_bytes) {
        return false;
    }

    index = free_indices_.back();
    Slot& slot = slots_[index];
    operation = encode_operation(index, slot.generation);
    return true;
}

void StoragePipeline::publish(PipelineReservation&& reservation, AcceptedRecord&& record) noexcept {
    StoragePipeline* pipeline = reservation.pipeline_;
    if (pipeline == nullptr || pipeline != this) {
        std::terminate();
    }
    const std::size_t index = reservation.index_;
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (slot.dispatch_state != DispatchState::Reserved ||
        reservation.payload_pool_reservation_.active()) {
        std::terminate();
    }
    slot.record.emplace(std::move(record));
    slot.payload_release_state = slot.record->record_payload().requires_external_release()
                                     ? PayloadReleaseState::Required
                                     : PayloadReleaseState::NotRequired;
    slot.payload_reservation = std::move(reservation.payload_reservation_);
    slot.dispatch_state = DispatchState::Ready;
    if (failure_cutoff_.load(std::memory_order_acquire)) {
        classify_never_handed_off_locked(index);
    } else {
        const std::size_t tail = (dispatch_head_ + dispatch_count_) % capacity_;
        dispatch_ring_[tail] = index;
        ++dispatch_count_;
    }
    reservation.pipeline_ = nullptr;
    reservation.record_reservation_.release();
    dispatch_condition_.notify_one();
}

void StoragePipeline::close_dispatch() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatch_closed_ = true;
    dispatch_condition_.notify_all();
}

void StoragePipeline::classify_never_handed_off_locked(std::size_t index) noexcept {
    Slot& slot = slots_[index];
    // Admission history is immutable. Only the independent Storage axes end.
    slot.submit_return = SubmitReturn::ReturnedError;
    slot.payload_axis = PayloadAxis::Consumed;
    slot.persistence_axis = PersistenceAxis::Failed;
    slot.dispatch_state = DispatchState::Terminalizing;
    if (!schedule_completion_locked(index)) {
        std::terminate();  // Pre-reserved completion capacity covers every slot.
    }
    submit_return_condition_.notify_all();
}

void StoragePipeline::begin_failure_cutoff() noexcept {
    failure_cutoff_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    admission_open_ = false;
    dispatch_closed_ = true;
    while (dispatch_count_ != 0) {
        const std::size_t index = dispatch_ring_[dispatch_head_];
        dispatch_head_ = (dispatch_head_ + 1U) % capacity_;
        --dispatch_count_;
        classify_never_handed_off_locked(index);
    }
    dispatch_condition_.notify_all();
    resource_condition_.notify_all();
}

void StoragePipeline::resolve_after_close() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // A returned, successful close is the no-future-access barrier. It proves
    // Payload safety, never a previously unconfirmed persistence success.
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        Slot& slot = slots_[index];
        if (!slot.operation_active || slot.submit_return == SubmitReturn::NotReturned) {
            continue;
        }
        if (slot.payload_axis == PayloadAxis::Pending && !slot.payload_event_seen) {
            slot.payload_axis = PayloadAxis::Consumed;
        }
        if (slot.persistence_axis == PersistenceAxis::Pending && !slot.persistence_event_seen) {
            slot.persistence_axis = PersistenceAxis::Unknown;
        }
        if (!schedule_completion_locked(index)) {
            std::terminate();
        }
    }
}

void StoragePipeline::close_callbacks() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_closed_ = true;
    stopping_ = true;
    completion_condition_.notify_all();
    watchdog_condition_.notify_all();
}

void StoragePipeline::close_release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    release_closed_ = true;
    release_condition_.notify_all();
}

void StoragePipeline::notify_admission_waiters() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    resource_condition_.notify_all();
}

void StoragePipeline::join_dispatch() noexcept {
    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }
}

void StoragePipeline::join_completion() noexcept {
    if (completion_thread_.joinable()) {
        completion_thread_.join();
    }
}

void StoragePipeline::join_release() noexcept {
    for (std::size_t index = 0; index < release_threads_.size(); ++index) {
        if (release_threads_[index].joinable()) {
            release_threads_[index].join();
            release_worker_states_[index]->joined->store(true, std::memory_order_release);
        }
    }
}

void StoragePipeline::reap_completed_workers() noexcept {
    if (dispatch_done_.load(std::memory_order_acquire) && dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }
    if (completion_done_.load(std::memory_order_acquire) && completion_thread_.joinable()) {
        completion_thread_.join();
    }
    if (watchdog_done_.load(std::memory_order_acquire) && watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
    for (std::size_t index = 0; index < release_threads_.size(); ++index) {
        if (release_worker_states_[index]->done->load(std::memory_order_acquire) &&
            release_threads_[index].joinable()) {
            release_threads_[index].join();
            release_worker_states_[index]->joined->store(true, std::memory_order_release);
        }
    }
}

bool StoragePipeline::retain_blocked_release_workers() noexcept {
    bool retained = true;
    for (std::size_t index = 0; index < release_threads_.size(); ++index) {
        std::thread& worker = release_threads_[index];
        if (!worker.joinable()) {
            continue;
        }
        const std::shared_ptr<ReleaseWorkerState>& state = release_worker_states_[index];
        if (state != nullptr && state->done->load(std::memory_order_acquire)) {
            worker.join();
            state->joined->store(true, std::memory_order_release);
            continue;
        }
        if (state == nullptr || !plugin::PluginQuarantine::process().retain_worker(
                                    std::move(worker), state, state->done,
                                    "Storage Payload release worker", state->joined)) {
            retained = false;
        }
    }
    return retained;
}

bool StoragePipeline::wait_dispatch_idle(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return dispatch_condition_.wait_until(lock, deadline, [this] {
        return dispatch_count_ == 0 && dispatch_active_generation_ == 0;
    });
}

bool StoragePipeline::wait_operations_idle(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return resource_condition_.wait_until(lock, deadline, [this] {
        return std::none_of(
            slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.operation_active; });
    });
}

bool StoragePipeline::wait_release_idle(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    // A transferred worker still executes against this Pipeline. Empty queues
    // alone do not establish its lifetime barrier; require the manager's Join.
    return release_condition_.wait_until(lock, deadline, [this] {
        if (release_count_ != 0 || release_active_count_ != 0) {
            return false;
        }
        for (std::size_t index = 0; index < release_threads_.size(); ++index) {
            if (!release_threads_[index].joinable() &&
                !release_worker_states_[index]->joined->load(std::memory_order_acquire)) {
                return false;
            }
        }
        return true;
    });
}

bool StoragePipeline::wait_storage_quiescent(std::chrono::steady_clock::time_point deadline) {
    return wait_dispatch_idle(deadline) && wait_release_idle(deadline) &&
           wait_operations_idle(deadline);
}

bool StoragePipeline::has_unresolved_operations() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(
        slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.operation_active; });
}

std::size_t StoragePipeline::active_operation_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        slots_.begin(), slots_.end(), [](const Slot& slot) { return slot.operation_active; }));
}

std::size_t StoragePipeline::ready_dispatch_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dispatch_count_;
}

void StoragePipeline::dispatch_loop() {
    while (true) {
        std::size_t index = kInvalidIndex;
        std::uint64_t call_generation = 0;
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
            slots_[index].dispatch_state = DispatchState::InFlight;
            ++dispatch_call_generation_;
            if (dispatch_call_generation_ == 0) {
                ++dispatch_call_generation_;
            }
            call_generation = dispatch_call_generation_;
            dispatch_active_generation_ = call_generation;
            dispatch_deadline_ = Clock::now() + progress_timeout_;
            dispatch_condition_.notify_all();
            watchdog_condition_.notify_one();
        }
        dispatch_one(index);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (dispatch_active_generation_ == call_generation) {
                dispatch_active_generation_ = 0;
            }
            dispatch_condition_.notify_all();
            watchdog_condition_.notify_all();
        }
    }
}

void StoragePipeline::watchdog_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        watchdog_condition_.wait(lock, [this] {
            return dispatch_active_generation_ != 0 ||
                   (stopping_ && dispatch_active_generation_ == 0);
        });
        if (dispatch_active_generation_ == 0) {
            return;
        }

        const std::uint64_t generation = dispatch_active_generation_;
        const auto deadline = dispatch_deadline_;
        if (!watchdog_condition_.wait_until(lock, deadline, [this, generation] {
                return dispatch_active_generation_ != generation ||
                       (stopping_ && dispatch_active_generation_ == 0);
            })) {
            if (dispatch_active_generation_ == generation &&
                dispatch_overdue_generation_ != generation) {
                dispatch_overdue_generation_ = generation;
                lock.unlock();
                fail(Status(
                    StatusCode::Timeout, "storage", 0,
                    "Storage submit exceeded its progress deadline"));
                lock.lock();
            }
            watchdog_condition_.wait(lock, [this, generation] {
                return dispatch_active_generation_ != generation ||
                       (stopping_ && dispatch_active_generation_ == 0);
            });
        }
    }
}

void StoragePipeline::dispatch_one(std::size_t index) {
    std::array<rodaq_timestamp_value, TimestampState::kMaxSlots> timestamp_values{};
    rodaq_storage_operation operation{};
    Result<rodaq_storage_submission> submission = rodaq::Status(
        StatusCode::InvalidState, "storage", 0, "Storage dispatch slot is unavailable");
    const AcceptedRecord* record = nullptr;
    std::int64_t accepted_steady_time_ns = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[index];
        if (!slot.operation_active || !slot.record.has_value()) {
            return;
        }
        operation = slot.operation;
        record = &slot.record.value();
        accepted_steady_time_ns = record->accepted_steady_time_ns();
    }
    try {
        submission = submission_builder_(*record, operation, timestamp_values);
    } catch (const std::exception& error) {
        submission = rodaq::Status(StatusCode::InternalError, "storage", 0, error.what());
    } catch (...) {
        submission = rodaq::Status(
            StatusCode::InternalError, "storage", 0, "Storage submission construction threw");
    }
    if (!submission.ok()) {
        observe_latency(StorageLatencyKind::Handoff, accepted_steady_time_ns);
        complete_submit(index, submission.status());
        fail(Status(
            StatusCode::InternalError, "storage", 0, "Storage submission construction failed"));
        return;
    }

    // This acquire is the handoff-entry linearization point. A call authorized
    // before cutoff retains its original thread/ownership until return. The
    // failure path alone needs the queue mutex to terminalize a skipped slot.
    if (failure_cutoff_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(mutex_);
        classify_never_handed_off_locked(index);
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
        result = rodaq::Status(StatusCode::InternalError, "storage", 0, error.what());
        observe_latency(StorageLatencyKind::Handoff, accepted_steady_time_ns);
        complete_submit(index, std::move(result));
        return;
    } catch (...) {
        active_submit_frame = previous;
        result = rodaq::Status(StatusCode::InternalError, "storage", 0, "Storage submit threw");
        observe_latency(StorageLatencyKind::Handoff, accepted_steady_time_ns);
        complete_submit(index, std::move(result));
        return;
    }
    active_submit_frame = previous;
    observe_latency(StorageLatencyKind::Handoff, accepted_steady_time_ns);
    complete_submit(index, std::move(result));
}

void StoragePipeline::complete_submit(
    std::size_t index, Result<rodaq_storage_submit_result> result) {
    CompletionEffects effects;
    std::int64_t accepted_steady_time_ns = 0;
    bool observed_payload_consumed = false;
    bool observed_persistence_completed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[index];
        if (!slot.operation_active || !slot.record.has_value()) {
            return;
        }
        accepted_steady_time_ns = slot.record->accepted_steady_time_ns();

        if (!result.ok()) {
            slot.submit_return = SubmitReturn::ReturnedError;
            if (slot.payload_event_seen || slot.persistence_event_seen) {
                apply_violation(
                    effects, "Storage emitted completion before returning a non-OK submit result");
            }
            effects.failure = std::move(result).status();
            slot.payload_axis = PayloadAxis::Consumed;
            slot.persistence_axis = PersistenceAxis::Failed;
            release_payload_locked(index, slot, effects);
            if (can_terminalize_locked(slot)) {
                terminalize_operation_locked(index, slot, effects);
            }
            submit_return_condition_.notify_all();
            resource_condition_.notify_all();
        } else {
            const rodaq_storage_submit_result& value = result.value();
            if ((value.payload_state != RODAQ_STORAGE_PAYLOAD_PENDING &&
                 value.payload_state != RODAQ_STORAGE_PAYLOAD_CONSUMED) ||
                (!is_terminal(value.persistence_state) &&
                 value.persistence_state != RODAQ_STORAGE_PERSISTENCE_PENDING)) {
                apply_violation(effects, "Storage returned an invalid submit state");
            }

            slot.submit_return = SubmitReturn::ReturnedOK;
            slot.handoff_accepted = true;
            if (value.payload_state == RODAQ_STORAGE_PAYLOAD_CONSUMED) {
                observed_payload_consumed = true;
                slot.payload_axis = PayloadAxis::Consumed;
                if (slot.payload_event_seen) {
                    apply_violation(
                        effects,
                        "Storage emitted PayloadConsumed for a synchronously consumed axis");
                }
                release_payload_locked(index, slot, effects);
            } else {
                slot.payload_axis = PayloadAxis::Pending;
            }

            switch (value.persistence_state) {
                case RODAQ_STORAGE_PERSISTENCE_SUCCEEDED:
                    observed_persistence_completed = true;
                    slot.persistence_axis = PersistenceAxis::Succeeded;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_FAILED:
                    observed_persistence_completed = true;
                    slot.persistence_axis = PersistenceAxis::Failed;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_UNKNOWN:
                    observed_persistence_completed = true;
                    slot.persistence_axis = PersistenceAxis::Unknown;
                    break;
                case RODAQ_STORAGE_PERSISTENCE_PENDING:
                    slot.persistence_axis = PersistenceAxis::Pending;
                    break;
                default:
                    slot.persistence_axis = PersistenceAxis::Unknown;
                    apply_violation(effects, "Storage returned an invalid persistence state");
                    break;
            }
            if (slot.persistence_axis != PersistenceAxis::Pending && slot.persistence_event_seen) {
                apply_violation(
                    effects, "Storage completed a synchronous-terminal persistence axis");
            }
            if (can_terminalize_locked(slot)) {
                terminalize_operation_locked(index, slot, effects);
            }
            submit_return_condition_.notify_all();
            resource_condition_.notify_all();
        }
    }

    if (observed_payload_consumed) {
        observe_latency(StorageLatencyKind::PayloadConsumed, accepted_steady_time_ns);
    }
    if (observed_persistence_completed) {
        observe_latency(StorageLatencyKind::PersistenceCompleted, accepted_steady_time_ns);
    }
    notify_payload_released(effects);
    notify_operation_completed(effects);
    if (!effects.violation.ok()) {
        fail(std::move(effects.violation));
    } else if (!effects.failure.ok()) {
        fail(std::move(effects.failure));
    }
    effects.payload_to_release = Payload();
    effects.record_to_destroy.reset();
}

void StoragePipeline::payload_consumed(rodaq_storage_operation operation) noexcept {
    Status violation;
    std::int64_t accepted_steady_time_ns = 0;
    try {
        if (active_submit_frame != nullptr && active_submit_frame->pipeline == this &&
            active_submit_frame->operation.value == operation.value) {
            violation = Status(
                StatusCode::InternalError, "storage", 0,
                "Storage emitted same-stack PayloadConsumed during submit");
            fail(std::move(violation));
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t index = 0;
        if (!find_operation_locked(operation, index)) {
            violation = Status(
                StatusCode::InternalError, "storage", 0,
                "Storage emitted PayloadConsumed for an unknown operation");
        } else if (callbacks_closed_) {
            violation = Status(
                StatusCode::InternalError, "storage", 0,
                "Storage emitted PayloadConsumed after callback quiescence");
        } else {
            Slot& slot = slots_[index];
            if (slot.payload_event_seen || slot.payload_axis == PayloadAxis::Consumed) {
                violation = Status(
                    StatusCode::InternalError, "storage", 0,
                    "Storage emitted duplicate PayloadConsumed");
            } else {
                slot.payload_event_seen = true;
                if (slot.record.has_value()) {
                    accepted_steady_time_ns = slot.record->accepted_steady_time_ns();
                }
                if (!schedule_completion_locked(index)) {
                    violation = Status(
                        StatusCode::InternalError, "storage", 0,
                        "Completion scheduling capacity was exceeded");
                }
            }
        }
    } catch (...) {
        violation = Status(
            StatusCode::InternalError, "storage", 0, "Storage PayloadConsumed staging failed");
    }
    if (!violation.ok()) {
        fail(std::move(violation));
    } else {
        observe_latency(StorageLatencyKind::PayloadConsumed, accepted_steady_time_ns);
    }
}

void StoragePipeline::persistence_completed(
    rodaq_storage_operation operation, rodaq_storage_persistence_state state,
    const rodaq_status* error) noexcept {
    Status violation;
    std::int64_t accepted_steady_time_ns = 0;
    try {
        if (active_submit_frame != nullptr && active_submit_frame->pipeline == this &&
            active_submit_frame->operation.value == operation.value) {
            violation = Status(
                StatusCode::InternalError, "storage", 0,
                "Storage emitted same-stack persistence completion during submit");
            fail(std::move(violation));
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t index = 0;
        if (!find_operation_locked(operation, index)) {
            violation = Status(
                StatusCode::InternalError, "storage", 0,
                "Storage completed an unknown persistence operation");
        } else if (callbacks_closed_) {
            violation = Status(
                StatusCode::InternalError, "storage", 0,
                "Storage completed persistence after callback quiescence");
        } else if (!is_terminal(state)) {
            violation = Status(
                StatusCode::InternalError, "storage", 0,
                "Storage completed persistence with an invalid state");
        } else {
            Slot& slot = slots_[index];
            if (slot.persistence_event_seen ||
                slot.persistence_axis == PersistenceAxis::Succeeded ||
                slot.persistence_axis == PersistenceAxis::Failed ||
                slot.persistence_axis == PersistenceAxis::Unknown) {
                violation = Status(
                    StatusCode::InternalError, "storage", 0,
                    "Storage emitted duplicate persistence completion");
            } else if (state == RODAQ_STORAGE_PERSISTENCE_SUCCEEDED && error != nullptr) {
                violation = Status(
                    StatusCode::InternalError, "storage", 0,
                    "Storage supplied a diagnostic for successful persistence");
            } else if (state != RODAQ_STORAGE_PERSISTENCE_SUCCEEDED && error == nullptr) {
                violation = Status(
                    StatusCode::InternalError, "storage", 0,
                    "Storage persistence failure diagnostic is missing");
            } else {
                slot.persistence_event_seen = true;
                slot.persistence_event_state = state;
                if (slot.record.has_value()) {
                    accepted_steady_time_ns = slot.record->accepted_steady_time_ns();
                }
                copy_diagnostic(error, slot);
                if (!schedule_completion_locked(index)) {
                    violation = Status(
                        StatusCode::InternalError, "storage", 0,
                        "Completion scheduling capacity was exceeded");
                }
            }
        }
    } catch (...) {
        violation = Status(
            StatusCode::InternalError, "storage", 0,
            "Storage persistence completion staging failed");
    }
    if (!violation.ok()) {
        fail(std::move(violation));
    } else {
        observe_latency(StorageLatencyKind::PersistenceCompleted, accepted_steady_time_ns);
    }
}

void StoragePipeline::completion_loop() {
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

        if (!wait_for_submit_return(index, Clock::now() + progress_timeout_)) {
            fail(Status(
                StatusCode::Timeout, "storage", 0,
                "Storage completion arrived but submit return was not reconciled"));
            continue;
        }
        CompletionEffects effects = process_completion(index);
        notify_payload_released(effects);
        notify_operation_completed(effects);
        if (!effects.violation.ok()) {
            fail(std::move(effects.violation));
        }
        effects.payload_to_release = Payload();
        effects.record_to_destroy.reset();
    }
}

void StoragePipeline::release_loop(const std::shared_ptr<ReleaseWorkerState>& state) {
    struct DoneGuard {
        const std::shared_ptr<ReleaseWorkerState>& state;
        ~DoneGuard() { state->done->store(true, std::memory_order_release); }
    } done_guard{state};

    while (true) {
        std::size_t index = kInvalidIndex;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            release_condition_.wait(
                lock, [this] { return release_count_ != 0 || release_closed_; });
            if (release_count_ == 0) {
                return;
            }
            index = release_ring_[release_head_];
            release_head_ = (release_head_ + 1U) % capacity_;
            --release_count_;
            Slot& slot = slots_[index];
            if (!slot.operation_active ||
                slot.payload_release_state != PayloadReleaseState::Queued) {
                continue;
            }
            slot.payload_release_state = PayloadReleaseState::Running;
            ++release_active_count_;
            release_condition_.notify_all();
        }

        if (!wait_for_submit_return(index, Clock::now() + progress_timeout_)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                Slot& slot = slots_[index];
                if (slot.payload_release_state == PayloadReleaseState::Running) {
                    slot.payload_release_state = PayloadReleaseState::Required;
                    --release_active_count_;
                }
                release_condition_.notify_all();
            }
            fail(Status(
                StatusCode::Timeout, "storage", 0,
                "Payload release waited for an unreconciled Storage submit"));
            continue;
        }

        {
            Payload payload;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                Slot& slot = slots_[index];
                if (!slot.operation_active ||
                    slot.payload_release_state != PayloadReleaseState::Running) {
                    --release_active_count_;
                    release_condition_.notify_all();
                    continue;
                }
                payload = std::move(slot.release_payload);
            }
            payload = Payload();
        }

        CompletionEffects effects;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Slot& slot = slots_[index];
            if (slot.payload_release_state == PayloadReleaseState::Running) {
                slot.payload_release_state = PayloadReleaseState::Released;
                slot.payload_reservation.release();
                effects.payload_released = true;
                effects.released_bytes = slot.payload_size;
                if (can_terminalize_locked(slot)) {
                    terminalize_operation_locked(index, slot, effects);
                }
                --release_active_count_;
                resource_condition_.notify_all();
                release_condition_.notify_all();
            }
        }
        notify_payload_released(effects);
        notify_operation_completed(effects);
        effects.record_to_destroy.reset();
    }
}

bool StoragePipeline::wait_for_submit_return(
    std::size_t index, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return submit_return_condition_.wait_until(lock, deadline, [this, index] {
        const Slot& slot = slots_[index];
        return !slot.operation_active || slot.submit_return != SubmitReturn::NotReturned;
    });
}

StoragePipeline::CompletionEffects StoragePipeline::process_completion(std::size_t index) {
    CompletionEffects effects;
    std::lock_guard<std::mutex> lock(mutex_);
    Slot& slot = slots_[index];
    if (!slot.operation_active || slot.submit_return == SubmitReturn::NotReturned) {
        return effects;
    }

    if (slot.payload_axis == PayloadAxis::Consumed &&
        (slot.payload_release_state == PayloadReleaseState::Required ||
         slot.payload_release_state == PayloadReleaseState::NotRequired)) {
        release_payload_locked(index, slot, effects);
    }
    if (slot.payload_event_seen) {
        slot.payload_event_seen = false;
        if (slot.payload_axis == PayloadAxis::Consumed) {
            apply_violation(effects, "PayloadConsumed callback was already terminal");
        } else {
            slot.payload_axis = PayloadAxis::Consumed;
            release_payload_locked(index, slot, effects);
        }
    }

    if (slot.persistence_event_seen) {
        slot.persistence_event_seen = false;
        if (slot.persistence_axis == PersistenceAxis::Succeeded ||
            slot.persistence_axis == PersistenceAxis::Failed ||
            slot.persistence_axis == PersistenceAxis::Unknown) {
            apply_violation(effects, "Persistence callback was already terminal");
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
                    apply_violation(effects, "Invalid staged persistence state");
                    break;
            }
        }
    }

    if (can_terminalize_locked(slot)) {
        terminalize_operation_locked(index, slot, effects);
    } else {
        slot.scheduled = false;
    }
    resource_condition_.notify_all();
    completion_condition_.notify_all();
    return effects;
}

void StoragePipeline::release_payload_locked(
    std::size_t index, Slot& slot, CompletionEffects& effects) noexcept {
    if (slot.payload_axis != PayloadAxis::Consumed || !slot.record.has_value()) {
        return;
    }
    if (slot.payload_release_state == PayloadReleaseState::Required) {
        slot.release_payload = slot.record->take_payload();
        if (!schedule_release_locked(index)) {
            apply_violation(effects, "Payload release scheduling capacity was exceeded");
        }
        return;
    }
    effects.payload_to_release = slot.record->take_payload();
    slot.payload_release_state = PayloadReleaseState::Released;
    slot.payload_reservation.release();
    effects.payload_released = true;
    effects.released_bytes = slot.payload_size;
    resource_condition_.notify_all();
}

bool StoragePipeline::can_terminalize_locked(const Slot& slot) const noexcept {
    const bool payload_terminal = slot.payload_axis == PayloadAxis::Consumed &&
                                  (slot.payload_release_state == PayloadReleaseState::NotRequired ||
                                   slot.payload_release_state == PayloadReleaseState::Released);
    return payload_terminal && slot.persistence_axis != PersistenceAxis::Pending &&
           slot.persistence_axis != PersistenceAxis::AwaitingReturn;
}

void StoragePipeline::terminalize_operation_locked(
    std::size_t index, Slot& slot, CompletionEffects& effects) noexcept {
    if (!slot.operation_active) {
        return;
    }
    if (slot.record.has_value()) {
        effects.record_to_destroy.emplace(std::move(slot.record.value()));
        slot.record.reset();
    }
    effects.operation_completed = true;
    switch (slot.persistence_axis) {
        case PersistenceAxis::Succeeded:
            effects.persistence = RODAQ_STORAGE_PERSISTENCE_SUCCEEDED;
            break;
        case PersistenceAxis::Failed:
            effects.persistence = RODAQ_STORAGE_PERSISTENCE_FAILED;
            break;
        default:
            effects.persistence = RODAQ_STORAGE_PERSISTENCE_UNKNOWN;
            break;
    }
    slot.payload_size = 0;
    slot.operation_active = false;
    slot.dispatch_state = DispatchState::Terminalizing;
    slot.scheduled = false;
    return_index_locked(index);
}

void StoragePipeline::notify_payload_released(const CompletionEffects& effects) noexcept {
    if (!effects.payload_released) {
        return;
    }
    try {
        payload_release_sink_(effects.released_bytes);
    } catch (...) {
        fail(Status(
            StatusCode::InternalError, "storage", 0,
            "Payload-release observer raised an exception"));
    }
}

void StoragePipeline::notify_operation_completed(const CompletionEffects& effects) noexcept {
    if (!effects.operation_completed) {
        return;
    }
    try {
        operation_complete_sink_(effects.persistence);
    } catch (...) {
        fail(Status(
            StatusCode::InternalError, "storage", 0,
            "Storage-completion observer raised an exception"));
    }
}

void StoragePipeline::observe_latency(StorageLatencyKind kind, std::int64_t accepted_ns) noexcept {
    if (accepted_ns <= 0) {
        return;
    }
    const auto now = Clock::now().time_since_epoch();
    const std::int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    if (now_ns < accepted_ns) {
        return;
    }
    try {
        latency_sink_(kind, now_ns - accepted_ns);
    } catch (...) {
        fail(Status(
            StatusCode::InternalError, "storage", 0,
            "Storage latency observer raised an exception"));
    }
}

void StoragePipeline::return_index_locked(std::size_t index) noexcept {
    Slot& slot = slots_[index];
    if (slot.dispatch_state != DispatchState::Terminalizing) {
        return;
    }
    slot.dispatch_state = DispatchState::Free;
    slot.operation_active = false;
    if (slot.generation == std::numeric_limits<std::uint32_t>::max()) {
        return;
    }
    ++slot.generation;
    free_indices_.push_back(index);
    resource_condition_.notify_all();
}

bool StoragePipeline::schedule_release_locked(std::size_t index) noexcept {
    Slot& slot = slots_[index];
    if (slot.payload_release_state == PayloadReleaseState::Queued ||
        slot.payload_release_state == PayloadReleaseState::Running ||
        slot.payload_release_state == PayloadReleaseState::Released) {
        return true;
    }
    if (slot.payload_release_state != PayloadReleaseState::Required ||
        release_count_ >= capacity_) {
        return false;
    }
    const std::size_t tail = (release_head_ + release_count_) % capacity_;
    release_ring_[tail] = index;
    ++release_count_;
    slot.payload_release_state = PayloadReleaseState::Queued;
    release_condition_.notify_one();
    return true;
}

bool StoragePipeline::find_operation_locked(
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

bool StoragePipeline::schedule_completion_locked(std::size_t index) noexcept {
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

void StoragePipeline::apply_violation(CompletionEffects& effects, const char* message) noexcept {
    if (effects.violation.ok()) {
        try {
            effects.violation = Status(StatusCode::InternalError, "storage", 0, message);
        } catch (...) {
        }
    }
}

void StoragePipeline::fail(Status error) noexcept {
    if (failure_reported_.exchange(true)) {
        return;
    }
    try {
        failure_sink_(std::move(error));
    } catch (...) {
    }
}

void StoragePipeline::copy_diagnostic(const rodaq_status* error, Slot& slot) noexcept {
    slot.persistence_diagnostic_present = error != nullptr;
    if (error != nullptr) {
        slot.persistence_diagnostic = *error;
    } else {
        slot.persistence_diagnostic = {};
    }
}

}  // namespace rodaq
