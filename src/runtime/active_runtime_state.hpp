#ifndef RODAQ_RUNTIME_ACTIVE_RUNTIME_STATE_HPP_
#define RODAQ_RUNTIME_ACTIVE_RUNTIME_STATE_HPP_

#include "runtime/failure_signal.hpp"

#include "plugin/service_registry.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include "runtime/clock_correlation.hpp"
#include "runtime/interop_projection_registry.hpp"
#include "runtime/logging_context.hpp"
#include "runtime/runtime.hpp"
#include "runtime/storage_pipeline.hpp"

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/runtime/latency_histogram.hpp>
#include <rodaq/runtime/payload_pool.hpp>
#include <rodaq/runtime/registry.hpp>
#include <rodaq/runtime/runtime_record.hpp>
#include <rodaq/runtime/runtime_stats.hpp>
#include <rodaq/runtime/structural_arena.hpp>

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

namespace rodaq::runtime {

using ArenaString = std::pmr::string;
using ArenaBytes = std::pmr::vector<std::uint8_t>;

struct SourceEntry {
    ArenaBytes source_id;
    std::size_t plugin_index = 0;
    std::unique_ptr<std::atomic<std::uint32_t>> availability;
};

struct ClockEntry {
    ArenaBytes native_id;
    std::uint64_t clock_id = 0;
    std::uint64_t resolution_numerator = 0;
    std::uint64_t resolution_denominator = 0;
    ArenaString origin;
    std::size_t plugin_index = 0;
};

struct SchemaEntry {
    std::uint64_t schema_id = 0;
    ArenaString format;
    bool type_identifier_present = false;
    ArenaString type_identifier;
    ArenaBytes representation;
};

struct TimestampKeyRecord {
    ArenaString role;
    ClockHandle clock;
};

struct TimestampKeySetEntry {
    std::uint64_t timestamp_key_set_id = 0;
    std::pmr::vector<TimestampKeyRecord> keys;
    std::pmr::vector<rodaq_timestamp_key> abi_keys;
};

struct StreamEntry {
    std::uint64_t stream_id = 0;
    std::size_t plugin_index = 0;
    ArenaBytes native_id;
    std::unique_ptr<std::atomic<std::uint32_t>> availability;
    std::unique_ptr<std::atomic<std::uint64_t>> current_binding;
    std::unique_ptr<std::mutex> admission_mutex;
    std::unique_ptr<std::mutex> binding_transition_mutex;
    std::unique_ptr<std::condition_variable> binding_transition_condition;
    bool binding_transition_active = false;
    std::uint64_t binding_transition_token = 0;
};

enum class DescriptorLifecycle : std::uint32_t {
    Published = 1,
    ReadinessPending = 2,
    Ready = 3,
    BindingPrepared = 4,
    Active = 5,
    Unsupported = 6,
    Failed = 7,
    Superseded = 8,
};

struct StreamDescriptorEntry {
    std::uint64_t stream_descriptor_id = 0;
    StreamHandle stream;
    bool schema_present = false;
    SchemaHandle schema;
    std::uint64_t schema_id = 0;
    ArenaString name;
    ArenaString payload_encoding;
    std::uint64_t timestamp_key_set_id = 0;
    std::pmr::vector<rodaq_timestamp_key> canonical_timestamp_keys;
    std::pmr::vector<rodaq_timestamp_key> transport_timestamp_keys;
    std::size_t plugin_index = 0;
    DescriptorLifecycle lifecycle = DescriptorLifecycle::Published;
    bool reconciliation_running = false;
    bool reconciliation_retry_requested = false;
};

enum class BindingLifecycle : std::uint32_t {
    Prepared = 1,
    Active = 2,
    Draining = 3,
    Retired = 4,
};

struct BindingEntry {
    StreamHandle stream;
    StreamDescriptorHandle descriptor;
    std::size_t plugin_index = 0;
    std::unique_ptr<std::atomic<std::uint32_t>> lifecycle;
    std::unique_ptr<std::atomic<std::uint64_t>> admission_inflight;
};

class ActiveRuntimeState;

struct AcquisitionServiceContext {
    ActiveRuntimeState* state = nullptr;
    std::size_t plugin_index = 0;
};

struct StorageServiceContext {
    ActiveRuntimeState* state = nullptr;
};

class ActiveRuntimeState final : public std::enable_shared_from_this<ActiveRuntimeState> {
public:
    static Result<std::unique_ptr<ActiveRuntimeState>> create(
        RuntimeConfig config, std::uint64_t activation_generation);

    ActiveRuntimeState(const ActiveRuntimeState&) = delete;
    ActiveRuntimeState& operator=(const ActiveRuntimeState&) = delete;
    ActiveRuntimeState(ActiveRuntimeState&&) = delete;
    ActiveRuntimeState& operator=(ActiveRuntimeState&&) = delete;
    ~ActiveRuntimeState() noexcept;

    RuntimeConfig& options() noexcept;
    const RuntimeConfig& options() const noexcept;
    std::uint64_t activation_generation() const noexcept;

    void set_acquisition_control(std::size_t plugin_index, rodaq_interface control);
    void set_storage_control(rodaq_interface control);
    void set_descriptor_progress_timeout(std::chrono::nanoseconds timeout);
    void set_binding_progress_timeout(std::chrono::nanoseconds timeout);
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
    RuntimeState state() const noexcept;
    bool gate_open() const noexcept;
    bool requires_quarantine() const noexcept;

    void reap_quarantined_workers() noexcept;
    std::shared_ptr<void> track_control_call();

    bool begin_submission() noexcept;
    void end_submission() noexcept;
    bool wait_for_admission_drain(std::chrono::steady_clock::time_point deadline);
    bool wait_for_submissions(std::chrono::steady_clock::time_point deadline);

    void set_failure_signal(std::shared_ptr<FailureSignal> signal) noexcept;
    void request_failure(Error error) noexcept;
    void record_secondary_failure(Error error) noexcept;
    bool has_primary_failure() const noexcept;
    Result<Error> primary_failure() const;
    Result<std::vector<Error>> secondary_failures() const;
    std::uint64_t secondary_failure_overflow_count() const noexcept;
    RuntimeStatsSnapshot stats() const noexcept;
    Result<PayloadPoolStats> payload_pool_statistics() const;
    void log(LogLevel level, std::string_view category, std::string_view message) noexcept;
    void diagnostic(
        rodaq_diagnostic_severity severity, std::string_view code,
        std::string_view message) noexcept;
    bool shutdown_logging(std::chrono::steady_clock::time_point deadline) noexcept;
    void quarantine_logging(std::string reason) noexcept;

    LoggingStats current_logging_stats() const noexcept;
    void record_pre_rejected(bool gate_closed, bool timeout) noexcept;
    void record_admission_wait(std::int64_t duration_ns) noexcept;
    void record_storage_latency(StorageLatencyKind kind, std::int64_t duration_ns) noexcept;
    void record_stop_latency() noexcept;

    Result<rodaq_runtime_lifecycle_snapshot> lifecycle_snapshot() const;

private:
    friend struct AcquisitionServiceContext;
    friend struct StorageServiceContext;

    ActiveRuntimeState(RuntimeConfig config, std::uint64_t activation_generation);

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
        const rodaq_error* diagnostic);

    Result<void> get_source(SourceHandle handle, rodaq_source_view& view) const;
    Result<void> get_clock(ClockHandle handle, rodaq_clock_view& view) const;
    Result<void> get_schema(SchemaHandle handle, rodaq_schema_view& view) const;
    Result<void> get_timestamp_key_set(std::uint64_t id, rodaq_timestamp_key_set_view& view) const;
    Result<void> get_stream(StreamHandle handle, rodaq_stream_view& view) const;
    Result<void> get_stream_descriptor(
        StreamDescriptorHandle handle, rodaq_stream_descriptor_view& view) const;

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

    static const rodaq_runtime_common_v1 kCommonVtable;
    static const rodaq_runtime_canonical_v1 kCanonicalVtable;
    static const rodaq_runtime_acquisition_v1 kAcquisitionVtable;
    static const rodaq_runtime_storage_v1 kStorageVtable;

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
    Result<BindingHandle> create_binding_locked(
        std::size_t plugin_index, StreamHandle stream, StreamDescriptorHandle descriptor,
        BindingLifecycle lifecycle);
    Result<void> initialize_ingress_clock();
    void fill_latency_stats(RuntimeStatsSnapshot& snapshot) const noexcept;
    Result<void> supervise_binding_control(
        std::function<Result<void>()> operation, const char* reason);

    RuntimeConfig options_;
    const std::uint64_t activation_generation_;
    std::unique_ptr<StructuralArena> arena_;
    Registry<SourceEntry, SourceHandle> sources_;
    Registry<ClockEntry, ClockHandle> clocks_;
    Registry<SchemaEntry, SchemaHandle> schemas_;
    Registry<TimestampKeySetEntry, TimestampKeySetHandle> timestamp_key_sets_;
    Registry<StreamEntry, StreamHandle> streams_;
    Registry<StreamDescriptorEntry, StreamDescriptorHandle> descriptors_;
    Registry<BindingEntry, BindingHandle> bindings_;
    AcceptanceSequenceAllocator sequences_;
    PayloadPool payload_pool_;
    std::unique_ptr<StoragePipeline> pipeline_;
    std::unique_ptr<LoggingContext> logging_context_;
    std::optional<plugin::StoragePluginInstance> storage_instance_;
    std::vector<std::optional<plugin::AcquisitionPluginInstance>> acquisition_instances_;
    std::vector<std::unique_ptr<std::atomic<std::uint32_t>>> acquisition_health_;

    mutable std::mutex control_mutex_;
    std::vector<SourceHandle> source_handles_;
    std::vector<ClockHandle> clock_handles_;
    std::vector<SchemaHandle> schema_handles_;
    std::vector<TimestampKeySetHandle> timestamp_key_set_handles_;
    std::vector<StreamHandle> stream_handles_;
    std::vector<StreamDescriptorHandle> descriptor_handles_;
    std::vector<BindingHandle> binding_handles_;

    std::uint64_t next_clock_id_ = 1;
    std::uint64_t next_schema_id_ = 1;
    std::uint64_t next_timestamp_key_set_id_ = 1;
    std::uint64_t next_stream_id_ = 1;
    std::uint64_t next_descriptor_id_ = 1;
    std::uint64_t next_transition_id_ = 1;

    std::vector<std::optional<rodaq_interface>> acquisition_controls_;
    std::optional<rodaq_interface> storage_control_;
    std::chrono::nanoseconds descriptor_progress_timeout_{};
    std::chrono::nanoseconds binding_progress_timeout_{};
    std::vector<std::unique_ptr<AcquisitionServiceContext>> acquisition_contexts_;
    std::unique_ptr<StorageServiceContext> storage_context_;
    InteropProjectionRegistry projections_;
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
    std::optional<Error> descriptor_worker_failure_;
    std::thread descriptor_worker_;

    std::atomic<RuntimeState> state_{RuntimeState::Constructed};
    mutable std::mutex admission_mutex_;
    std::condition_variable admission_condition_;
    bool gate_open_ = false;
    std::atomic<std::uint64_t> admission_eligibility_generation_{0};
    std::uint64_t active_submission_calls_ = 0;
    std::uint64_t admission_inflight_ = 0;
    std::atomic<std::size_t> active_control_calls_{0};
    std::shared_ptr<FailureSignal> failure_signal_;
    std::atomic<bool> control_quarantine_required_{false};

    mutable std::mutex failure_mutex_;
    std::optional<Error> primary_failure_;
    std::vector<Error> secondary_failures_;
    std::uint64_t secondary_overflow_count_ = 0;
    RuntimeStats stats_;
    std::unique_ptr<LatencyHistogram> admission_wait_latency_;
    std::unique_ptr<LatencyHistogram> storage_handoff_latency_;
    std::unique_ptr<LatencyHistogram> payload_consumed_latency_;
    std::unique_ptr<LatencyHistogram> persistence_latency_;
    std::unique_ptr<LatencyHistogram> stop_latency_;
    std::chrono::steady_clock::time_point stop_started_{};
    bool stop_started_valid_ = false;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_ACTIVE_RUNTIME_STATE_HPP_
