#ifndef RODAQ_RUNTIME_STORAGE_PIPELINE_HPP_
#define RODAQ_RUNTIME_STORAGE_PIPELINE_HPP_

#include "plugin/storage_plugin_instance.hpp"

#include <rodaq/result.hpp>
#include <rodaq/runtime/admission.hpp>
#include <rodaq/runtime/resource_budget.hpp>
#include <rodaq/runtime/runtime_record.hpp>

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

struct rodaq_error;

namespace rodaq::runtime {

class StoragePipeline;

enum class PreGateAcquireOutcome {
    Acquired,
    ResourceUnavailable,
    Timeout,
    GateClosed,
};

class PipelineReservation final {
public:
    PipelineReservation() noexcept = default;
    PipelineReservation(const PipelineReservation&) = delete;
    PipelineReservation& operator=(const PipelineReservation&) = delete;
    PipelineReservation(PipelineReservation&& other) noexcept;
    PipelineReservation& operator=(PipelineReservation&& other) noexcept;
    ~PipelineReservation();

    bool Active() const noexcept { return pipeline_ != nullptr; }
    std::size_t Index() const noexcept { return index_; }
    rodaq_storage_operation Operation() const noexcept { return operation_; }

    Reservation TakeRecordReservation() noexcept;
    Reservation TakePayloadReservation() noexcept;

private:
    friend class StoragePipeline;

    PipelineReservation(
        StoragePipeline& pipeline, std::size_t index, rodaq_storage_operation operation,
        Reservation record_reservation, Reservation payload_reservation) noexcept;

    void Cancel() noexcept;

    StoragePipeline* pipeline_ = nullptr;
    std::size_t index_ = 0;
    rodaq_storage_operation operation_{};
    Reservation record_reservation_;
    Reservation payload_reservation_;
};

struct PreGateAcquireResult {
    PreGateAcquireOutcome outcome = PreGateAcquireOutcome::ResourceUnavailable;
    PipelineReservation reservation;
};

class StoragePipeline final {
public:
    using FailureSink = std::function<void(Error)>;
    using CompletionSink = std::function<void(std::size_t)>;
    using SubmissionBuilder = std::function<Result<rodaq_storage_submission>(
        const RuntimeRecord&, rodaq_storage_operation,
        std::array<rodaq_timestamp_value, TimestampState::kMaxSlots>&)>;

    StoragePipeline(
        std::size_t capacity, std::size_t maximum_payload_bytes,
        plugin::StoragePluginInstance* storage, std::chrono::nanoseconds progress_timeout,
        FailureSink failure_sink, CompletionSink completion_sink,
        SubmissionBuilder submission_builder);

    StoragePipeline(const StoragePipeline&) = delete;
    StoragePipeline& operator=(const StoragePipeline&) = delete;
    StoragePipeline(StoragePipeline&&) = delete;
    StoragePipeline& operator=(StoragePipeline&&) = delete;
    ~StoragePipeline();

    Result<void> Start();
    void OpenAdmission() noexcept;
    void CloseAdmission() noexcept;
    bool AdmissionOpen() const noexcept;

    Result<PreGateAcquireResult> AcquirePreGate(
        std::size_t payload_bytes, AdmissionMode mode,
        std::chrono::steady_clock::time_point deadline);

    void Publish(PipelineReservation&& reservation, RuntimeRecord&& record) noexcept;

    void CloseDispatch() noexcept;
    void CloseCallbacks() noexcept;
    void JoinDispatch() noexcept;
    void JoinCompletion() noexcept;
    bool WaitDispatchIdle(std::chrono::steady_clock::time_point deadline);
    bool WaitOperationsIdle(std::chrono::steady_clock::time_point deadline);
    bool WaitStorageQuiescent(std::chrono::steady_clock::time_point deadline);
    bool HasUnresolvedOperations() const noexcept;

    void payload_consumed(rodaq_storage_operation operation) noexcept;
    void persistence_completed(
        rodaq_storage_operation operation, rodaq_storage_persistence_state state,
        const rodaq_error* error) noexcept;

    std::size_t ActiveOperationCount() const noexcept;
    std::size_t ReadyDispatchCount() const noexcept;

private:
    friend class PipelineReservation;

    struct Slot;

    enum class PayloadAxis : std::uint8_t {
        AwaitingReturn,
        Pending,
        Consumed,
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
        std::optional<RuntimeRecord> record_to_destroy;
        std::optional<Error> failure;
        std::optional<Error> violation;
        bool operation_completed = false;
        std::size_t completed_bytes = 0;
    };

    static constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t kMaxDiagnosticBytes = 512;

    bool TryAcquireLocked(
        std::size_t payload_bytes, std::size_t& index, rodaq_storage_operation& operation);
    void ReturnIndexLocked(std::size_t index) noexcept;
    bool FindOperationLocked(rodaq_storage_operation operation, std::size_t& index) noexcept;
    void ScheduleCompletionLocked(std::size_t index) noexcept;
    CompletionEffects ProcessCompletion(std::size_t index);
    void ApplyViolation(CompletionEffects& effects, const char* message) noexcept;
    void ReleasePayloadLocked(Slot& slot, CompletionEffects& effects) noexcept;
    void TerminalizeOperationLocked(
        std::size_t index, Slot& slot, CompletionEffects& effects) noexcept;
    void NotifyOperationCompleted(const CompletionEffects& effects) noexcept;
    void DispatchLoop();
    void DispatchOne(std::size_t index);
    void CompletionLoop();
    void CompleteSubmit(std::size_t index, Result<rodaq_storage_submit_result> result);
    bool WaitForSubmitReturn(std::size_t index, std::chrono::steady_clock::time_point deadline);
    void Fail(Error error) noexcept;
    void CopyDiagnostic(const rodaq_error* error, Slot& slot) noexcept;

    const std::size_t capacity_;
    const std::size_t maximum_payload_bytes_;
    const std::chrono::nanoseconds progress_timeout_;
    plugin::StoragePluginInstance* storage_;
    FailureSink failure_sink_;
    CompletionSink completion_sink_;
    SubmissionBuilder submission_builder_;

    ResourceBudget record_budget_;
    ResourceBudget payload_budget_;

    mutable std::mutex mutex_;
    std::condition_variable resource_condition_;
    std::condition_variable dispatch_condition_;
    std::condition_variable completion_condition_;
    std::condition_variable submit_return_condition_;

    std::vector<Slot> slots_;
    std::vector<std::size_t> free_indices_;
    std::vector<std::size_t> dispatch_ring_;
    std::vector<std::size_t> completion_ring_;
    std::size_t dispatch_head_ = 0;
    std::size_t dispatch_count_ = 0;
    bool dispatch_call_active_ = false;
    std::size_t completion_head_ = 0;
    std::size_t completion_count_ = 0;

    bool admission_open_ = false;
    bool dispatch_closed_ = false;
    bool callbacks_closed_ = false;
    bool stopping_ = false;
    bool started_ = false;
    std::atomic<bool> failure_reported_{false};
    std::thread dispatch_thread_;
    std::thread completion_thread_;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_STORAGE_PIPELINE_HPP_
