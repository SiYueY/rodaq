#ifndef RODAQ_ACTIVATION_ACTIVATION_STATE_HPP_
#define RODAQ_ACTIVATION_ACTIVATION_STATE_HPP_

#include "daq/failure_mailbox.hpp"

#include "activation/activation_limits.hpp"
#include "activation/activation_phase.hpp"
#include "admission/admission_state.hpp"
#include "binding/binding_state.hpp"
#include "canonical/canonical_state.hpp"
#include "canonical/model.hpp"
#include "canonical/timestamp_state.hpp"
#include "plugin/service_registry.hpp"
#include "plugin/service_context.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include "admission/acceptance_sequence.hpp"
#include "clock/clock_correlation.hpp"
#include "projection/projection_registry.hpp"
#include "logging/logging_context.hpp"
#include "metrics/activation_metrics.hpp"
#include <rodaq/daq.hpp>
#include "metrics/stats_accumulator.hpp"
#include "storage/storage_pipeline.hpp"

#include <rodaq/plugin/abi/runtime.h>
#include <metrics/latency_histogram.hpp>
#include <payload/payload_pool.hpp>
#include <canonical/registry.hpp>
#include <record/accepted_record.hpp>
#include <rodaq/daq_stats.hpp>
#include <activation/structural_arena.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rodaq {

class ActivationState;

class ActivationState final : public std::enable_shared_from_this<ActivationState> {
public:
    static Result<std::unique_ptr<ActivationState>> create(
        DAQConfig config, std::uint64_t activation_generation);

    ActivationState(const ActivationState&) = delete;
    ActivationState& operator=(const ActivationState&) = delete;
    ActivationState(ActivationState&&) = delete;
    ActivationState& operator=(ActivationState&&) = delete;
    ~ActivationState() noexcept;

    const DAQConfig& config() const noexcept;
    const ActivationLimits& limits() const noexcept;
    void apply_limits(const ActivationLimits& limits) noexcept;
    std::uint64_t activation_generation() const noexcept;

    void set_acquisition_control(std::size_t plugin_index, rodaq_interface control);
    void set_storage_control(rodaq_interface control);
    Result<rodaq_storage_capabilities> storage_capabilities() const;

    Result<void> initialize_pipeline(
        std::size_t effective_payload_limit, std::size_t maximum_payload_inflight_bytes,
        plugin::StoragePluginInstance* storage, std::chrono::nanoseconds progress_timeout);
    StoragePipeline* pipeline() noexcept;
    std::optional<plugin::StoragePluginInstance>& storage_instance() noexcept;
    std::optional<plugin::AcquisitionPluginInstance>& acquisition_instance(
        std::size_t index) noexcept;

    Result<void> register_acquisition_services(
        plugin::ServiceRegistry& registry, std::size_t plugin_index);
    Result<void> register_storage_services(plugin::ServiceRegistry& registry);

    bool mark_running() noexcept;
    void mark_stopping() noexcept;
    void mark_stopped() noexcept;
    void mark_failed() noexcept;
    ActivationPhase phase() const noexcept;
    bool gate_open() const noexcept;
    bool requires_quarantine() const noexcept;

    void reap_quarantined_workers() noexcept;
    std::shared_ptr<void> track_control_call();

    bool begin_submission() noexcept;
    void end_submission() noexcept;
    bool wait_for_admission_drain(std::chrono::steady_clock::time_point deadline);
    bool wait_for_submissions(std::chrono::steady_clock::time_point deadline);

    void set_failure_signal(std::shared_ptr<FailureMailbox> signal) noexcept;
    void request_failure(Status error) noexcept;
    void record_secondary_failure(Status error) noexcept;
    bool has_primary_failure() const noexcept;
    Status primary_failure() const;
    Result<std::vector<Status>> secondary_failures() const;
    std::uint64_t secondary_failure_overflow_count() const noexcept;
    DAQStats stats() const noexcept;
    Result<PayloadStats> payload_pool_statistics() const;
    void log(LogLevel level, std::string_view category, std::string_view message) noexcept;
    void diagnostic(
        rodaq_diagnostic_severity severity, std::string_view code,
        std::string_view message) noexcept;
    bool shutdown_logging(std::chrono::steady_clock::time_point deadline) noexcept;
    void quarantine_logging(std::string reason) noexcept;

    Result<Logger> make_logger(std::string_view category) const;
    LoggingStats current_logging_stats() const noexcept;
    void record_pre_rejected(bool gate_closed, bool timeout) noexcept;
    void record_admission_wait(std::int64_t duration_ns) noexcept;
    void record_storage_latency(StorageLatencyKind kind, std::int64_t duration_ns) noexcept;
    void record_stop_latency() noexcept;

    Result<DAQLifecycleSnapshot> lifecycle_snapshot() const;

private:
    friend struct AcquisitionServiceContext;
    friend struct StorageServiceContext;

    ActivationState(DAQConfig config, std::uint64_t activation_generation);

public:
    Result<void> register_source(
        std::size_t plugin_index, const rodaq_source_registration& request, SourceHandle& handle);
    Result<void> register_clock(
        std::size_t plugin_index, const rodaq_clock_registration& request, ClockHandle& handle);
    Result<void> register_schema(
        std::size_t plugin_index, const rodaq_schema_registration& request, SchemaHandle& handle);
    Result<void> register_stream(
        std::size_t plugin_index, const rodaq_stream_registration& request, StreamHandle& handle);
    Result<void> publish_stream_descriptor(
        std::size_t plugin_index, const rodaq_stream_descriptor_registration& request,
        StreamDescriptorHandle& handle);
    Result<rodaq_acquisition_submit_result> submit(
        std::size_t plugin_index, const rodaq_record_submission& submission,
        rodaq_payload_disposition& disposition);
    Result<void> report_source_availability(
        std::size_t plugin_index, SourceHandle source, rodaq_availability_state state);
    Result<void> report_stream_availability(
        std::size_t plugin_index, StreamHandle stream, rodaq_availability_state state);
    Result<void> report_plugin_state(
        std::size_t plugin_index, rodaq_acquisition_plugin_state state,
        const rodaq_status* diagnostic);

    Result<void> get_source(SourceHandle handle, rodaq_source_info& view) const;
    Result<void> get_clock(ClockHandle handle, rodaq_clock_info& view) const;
    Result<void> get_schema(SchemaHandle handle, rodaq_schema_info& view) const;
    Result<void> get_timestamp_key_set(std::uint64_t id, rodaq_timestamp_key_set_info& view) const;
    Result<void> get_stream(StreamHandle handle, rodaq_stream_info& view) const;
    Result<void> get_stream_descriptor(
        StreamDescriptorHandle handle, rodaq_stream_descriptor_info& view) const;

    void retire_draining_binding(BindingHandle binding) noexcept;
    void notify_admission_eligibility_changed() noexcept;
    rodaq_interface acquisition_service_interface(std::size_t plugin_index) noexcept;
    rodaq_interface storage_service_interface() noexcept;

    Result<void> start_descriptor_reconciliation();
    Result<void> wait_for_descriptor_reconciliation(std::chrono::steady_clock::time_point deadline);
    bool close_and_join_descriptor_reconciliation(std::chrono::steady_clock::time_point deadline);

private:
    enum class DescriptorReadiness {
        Ready,
        Unsupported,
        Failed,
    };

    static const rodaq_runtime_common kCommonVtable;
    static const rodaq_runtime_canonical kCanonicalVtable;
    static const rodaq_runtime_acquisition kAcquisitionVtable;
    static const rodaq_runtime_storage kStorageVtable;

    Result<DescriptorReadiness> ensure_descriptor_ready(StreamDescriptorHandle descriptor);
    Result<bool> reconcile_descriptor(StreamDescriptorHandle descriptor);
    Result<void> request_descriptor_reconciliation(
        StreamDescriptorHandle descriptor, bool critical_for_start);
    void run_descriptor_reconciliation() noexcept;
    Result<void> activate_descriptor_binding(
        StreamDescriptorHandle descriptor, StreamHandle stream, std::size_t plugin_index,
        rodaq_interface control);
    void on_projection_registered(
        rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) noexcept;
    Result<void> initialize_ingress_clock();
    void fill_latency_stats(DAQStats& snapshot) const noexcept;
    Result<void> supervise_binding_control(
        std::function<Result<void>()> operation, const char* reason);

    const DAQConfig config_;
    ActivationLimits limits_;
    const std::uint64_t activation_generation_;
    std::unique_ptr<StructuralArena> arena_;
    CanonicalState canonical_;
    BindingState bindings_;
    AdmissionState admission_;
    ActivationMetrics metrics_;
    PayloadPool payload_pool_;
    std::unique_ptr<StoragePipeline> pipeline_;
    std::unique_ptr<LoggingContext> logging_context_;
    std::optional<plugin::StoragePluginInstance> storage_instance_;
    std::vector<std::optional<plugin::AcquisitionPluginInstance>> acquisition_instances_;
    std::vector<std::unique_ptr<std::atomic<std::uint32_t>>> acquisition_health_;

    mutable std::mutex control_mutex_;
    std::vector<std::optional<rodaq_interface>> acquisition_controls_;
    std::optional<rodaq_interface> storage_control_;
    std::vector<std::unique_ptr<plugin::AcquisitionServiceContext>> acquisition_contexts_;
    std::unique_ptr<plugin::StorageServiceContext> storage_context_;
    ProjectionRegistry projections_;
    ClockHandle ingress_clock_;
    std::optional<ClockCorrelation> ingress_clock_correlation_;

    struct DescriptorWorkerRequest {
        StreamDescriptorHandle descriptor;
        bool critical_for_start = false;
    };

    std::mutex descriptor_worker_mutex_;
    std::condition_variable descriptor_worker_condition_;
    std::vector<DescriptorWorkerRequest> descriptor_worker_requests_;
    std::size_t descriptor_worker_head_ = 0;
    std::size_t descriptor_worker_count_ = 0;
    bool descriptor_worker_started_ = false;
    bool descriptor_worker_busy_ = false;
    bool descriptor_worker_closing_ = false;
    Status descriptor_worker_failure_;
    std::thread descriptor_worker_;

    std::atomic<ActivationPhase> phase_{ActivationPhase::Starting};
    std::atomic<std::size_t> active_control_calls_{0};
    std::shared_ptr<FailureMailbox> failure_signal_;
    std::atomic<bool> control_quarantine_required_{false};

    mutable std::mutex failure_mutex_;
    Status primary_failure_;
    std::vector<Status> secondary_failures_;
    std::uint64_t secondary_overflow_count_ = 0;
};

}  // namespace rodaq

#endif  // RODAQ_ACTIVATION_ACTIVATION_STATE_HPP_
