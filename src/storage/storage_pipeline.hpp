#ifndef RODAQ_STORAGE_PIPELINE_HPP_
#define RODAQ_STORAGE_PIPELINE_HPP_

#include "plugin/storage_plugin_instance.hpp"

#include <rodaq/result.hpp>
#include "admission/admission.hpp"
#include "payload/payload_pool.hpp"
#include "admission/resource_budget.hpp"
#include "record/accepted_record.hpp"

#include <chrono>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct rodaq_status;

namespace rodaq {

class StoragePipeline;

enum class StorageLatencyKind {
    Handoff,
    PayloadConsumed,
    PersistenceCompleted,
};

enum class PreGateAcquireOutcome {
    Acquired,
    ResourceUnavailable,
    Timeout,
    GateClosed,
    EligibilityChanged,
};

class PipelineReservation final {
public:
    PipelineReservation() noexcept = default;
    PipelineReservation(const PipelineReservation&) = delete;
    PipelineReservation& operator=(const PipelineReservation&) = delete;
    PipelineReservation(PipelineReservation&& other) noexcept;
    PipelineReservation& operator=(PipelineReservation&& other) noexcept;
    ~PipelineReservation();

    bool active() const noexcept;
    std::size_t index() const noexcept;
    rodaq_storage_operation operation() const noexcept;

    Reservation take_record_reservation() noexcept;
    Reservation take_payload_reservation() noexcept;
    PayloadPoolReservation take_payload_pool_reservation() noexcept;

private:
    friend class StoragePipeline;

    PipelineReservation(
        StoragePipeline& pipeline, std::size_t index, rodaq_storage_operation operation,
        Reservation record_reservation, Reservation payload_reservation,
        PayloadPoolReservation payload_pool_reservation) noexcept;

    void cancel() noexcept;

    StoragePipeline* pipeline_ = nullptr;
    std::size_t index_ = 0;
    rodaq_storage_operation operation_{};
    Reservation record_reservation_;
    Reservation payload_reservation_;
    PayloadPoolReservation payload_pool_reservation_;
};

struct PreGateAcquireResult {
    PreGateAcquireOutcome outcome = PreGateAcquireOutcome::ResourceUnavailable;
    PipelineReservation reservation;
};

class StoragePipeline final {
public:
    using FailureSink = std::function<void(Status)>;
    using PayloadReleaseSink = std::function<void(std::size_t)>;
    using OperationCompleteSink = std::function<void(rodaq_storage_persistence_state)>;
    using LatencySink = std::function<void(StorageLatencyKind, std::int64_t)>;
    using SubmissionBuilder = std::function<Result<rodaq_storage_submission>(
        const AcceptedRecord&, rodaq_storage_operation,
        std::array<rodaq_timestamp_value, TimestampState::kMaxSlots>&)>;

    StoragePipeline(
        std::size_t capacity, std::size_t completion_capacity, std::size_t maximum_payload_bytes,
        std::size_t maximum_payload_inflight_bytes, plugin::StoragePluginInstance* storage,
        std::chrono::nanoseconds progress_timeout, std::size_t release_worker_count,
        FailureSink failure_sink, PayloadReleaseSink payload_release_sink,
        OperationCompleteSink operation_complete_sink, LatencySink latency_sink,
        SubmissionBuilder submission_builder);

    StoragePipeline(const StoragePipeline&) = delete;
    StoragePipeline& operator=(const StoragePipeline&) = delete;
    StoragePipeline(StoragePipeline&&) = delete;
    StoragePipeline& operator=(StoragePipeline&&) = delete;
    ~StoragePipeline();

    Result<void> start();
    void open_admission() noexcept;
    void close_admission() noexcept;
    bool admission_open() const noexcept;

    Result<PreGateAcquireResult> acquire_pre_gate(
        std::size_t payload_bytes, AdmissionMode mode, PayloadPool* copy_pool, bool copy_payload,
        std::chrono::steady_clock::time_point deadline,
        const std::atomic<std::uint64_t>& eligibility_generation);

    void publish(PipelineReservation&& reservation, AcceptedRecord&& record) noexcept;

    void close_dispatch() noexcept;
    void begin_failure_cutoff() noexcept;
    void resolve_after_close() noexcept;
    void close_callbacks() noexcept;
    void close_release() noexcept;
    void notify_admission_waiters() noexcept;
    void join_dispatch() noexcept;
    void join_completion() noexcept;
    void join_release() noexcept;
    void reap_completed_workers() noexcept;
    bool retain_blocked_release_workers() noexcept;
    bool wait_dispatch_idle(std::chrono::steady_clock::time_point deadline);
    bool wait_operations_idle(std::chrono::steady_clock::time_point deadline);
    bool wait_release_idle(std::chrono::steady_clock::time_point deadline);
    bool wait_storage_quiescent(std::chrono::steady_clock::time_point deadline);
    bool has_unresolved_operations() const noexcept;

    void payload_consumed(rodaq_storage_operation operation) noexcept;
    void persistence_completed(
        rodaq_storage_operation operation, rodaq_storage_persistence_state state,
        const rodaq_status* error) noexcept;

    std::size_t active_operation_count() const noexcept;
    std::size_t ready_dispatch_count() const noexcept;

private:
    friend class PipelineReservation;

    struct Slot;
    struct ReleaseWorkerState {
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> joined = std::make_shared<std::atomic<bool>>(false);
    };

    enum class DispatchState : std::uint8_t {
        Free,
        Reserved,
        Ready,
        InFlight,
        Terminalizing,
    };

    enum class PayloadAxis : std::uint8_t {
        AwaitingReturn,
        Pending,
        Consumed,
    };

    enum class PayloadReleaseState : std::uint8_t {
        NotRequired,
        Required,
        Queued,
        Running,
        Released,
    };

    enum class PersistenceAxis : std::uint8_t {
        AwaitingReturn,
        Pending,
        Succeeded,
        Failed,
        Unknown,
    };

    enum class SubmitReturn : std::uint8_t {
        NotReturned,
        ReturnedOK,
        ReturnedError,
    };

    struct CompletionEffects {
        Payload payload_to_release;
        std::optional<AcceptedRecord> record_to_destroy;
        Status failure;
        Status violation;
        bool payload_released = false;
        std::size_t released_bytes = 0;
        bool operation_completed = false;
        rodaq_storage_persistence_state persistence = RODAQ_STORAGE_PERSISTENCE_UNKNOWN;
    };

    static constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t kMaxDiagnosticBytes = 512;

    bool try_acquire_locked(
        std::size_t payload_bytes, std::size_t& index, rodaq_storage_operation& operation);
    void classify_never_handed_off_locked(std::size_t index) noexcept;
    void return_index_locked(std::size_t index) noexcept;
    bool find_operation_locked(rodaq_storage_operation operation, std::size_t& index) noexcept;
    bool schedule_completion_locked(std::size_t index) noexcept;
    bool schedule_release_locked(std::size_t index) noexcept;
    CompletionEffects process_completion(std::size_t index);
    void apply_violation(CompletionEffects& effects, const char* message) noexcept;
    void release_payload_locked(std::size_t index, Slot& slot, CompletionEffects& effects) noexcept;
    bool can_terminalize_locked(const Slot& slot) const noexcept;
    void terminalize_operation_locked(
        std::size_t index, Slot& slot, CompletionEffects& effects) noexcept;
    void notify_payload_released(const CompletionEffects& effects) noexcept;
    void notify_operation_completed(const CompletionEffects& effects) noexcept;
    void observe_latency(StorageLatencyKind kind, std::int64_t accepted_ns) noexcept;
    void dispatch_loop();
    void watchdog_loop();
    void release_loop(const std::shared_ptr<ReleaseWorkerState>& state);
    void dispatch_one(std::size_t index);
    void completion_loop();
    void complete_submit(std::size_t index, Result<rodaq_storage_submit_result> result);
    bool wait_for_submit_return(std::size_t index, std::chrono::steady_clock::time_point deadline);
    void fail(Status error) noexcept;
    void copy_diagnostic(const rodaq_status* error, Slot& slot) noexcept;

    const std::size_t capacity_;
    const std::size_t completion_capacity_;
    const std::size_t maximum_payload_bytes_;
    const std::chrono::nanoseconds progress_timeout_;
    const std::size_t release_worker_count_;
    plugin::StoragePluginInstance* storage_;
    FailureSink failure_sink_;
    PayloadReleaseSink payload_release_sink_;
    OperationCompleteSink operation_complete_sink_;
    LatencySink latency_sink_;
    SubmissionBuilder submission_builder_;

    ResourceBudget record_budget_;
    ResourceBudget payload_budget_;

    mutable std::mutex mutex_;
    std::condition_variable resource_condition_;
    std::condition_variable dispatch_condition_;
    std::condition_variable completion_condition_;
    std::condition_variable release_condition_;
    std::condition_variable submit_return_condition_;
    std::condition_variable watchdog_condition_;

    std::vector<Slot> slots_;
    std::vector<std::size_t> free_indices_;
    std::vector<std::size_t> dispatch_ring_;
    std::vector<std::size_t> completion_ring_;
    std::vector<std::size_t> release_ring_;
    std::size_t dispatch_head_ = 0;
    std::size_t dispatch_count_ = 0;
    std::uint64_t dispatch_call_generation_ = 0;
    std::uint64_t dispatch_active_generation_ = 0;
    std::uint64_t dispatch_overdue_generation_ = 0;
    std::chrono::steady_clock::time_point dispatch_deadline_{};
    std::size_t completion_head_ = 0;
    std::size_t completion_count_ = 0;
    std::size_t release_head_ = 0;
    std::size_t release_count_ = 0;
    std::size_t release_active_count_ = 0;

    bool admission_open_ = false;
    bool dispatch_closed_ = false;
    std::atomic<bool> failure_cutoff_{false};
    bool callbacks_closed_ = false;
    bool release_closed_ = false;
    std::atomic<bool> stopping_{false};
    bool started_ = false;
    std::atomic<bool> failure_reported_{false};
    std::atomic<bool> dispatch_done_{false};
    std::atomic<bool> completion_done_{false};
    std::atomic<bool> watchdog_done_{false};
    std::thread dispatch_thread_;
    std::thread completion_thread_;
    std::vector<std::thread> release_threads_;
    std::vector<std::shared_ptr<ReleaseWorkerState>> release_worker_states_;
    std::thread watchdog_thread_;
};

}  // namespace rodaq

#endif  // RODAQ_STORAGE_PIPELINE_HPP_
