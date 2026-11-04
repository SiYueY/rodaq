#ifndef RODAQ_RUNTIME_ACTIVE_RUNTIME_STATE_HPP_
#define RODAQ_RUNTIME_ACTIVE_RUNTIME_STATE_HPP_

#include "plugin/service_registry.hpp"
#include "plugin/acquisition_plugin_instance.hpp"
#include "plugin/storage_plugin_instance.hpp"
#include "runtime/clock_correlation.hpp"
#include "runtime/interop_projection_registry.hpp"
#include "runtime/runtime.hpp"
#include "runtime/storage_pipeline.hpp"

#include <rodaq/plugin/abi/runtime.h>
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
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rodaq::runtime {

using ArenaString = std::pmr::string;
using ArenaBytes = std::pmr::vector<std::uint8_t>;

struct SourceEntry {
    ArenaBytes source_id;
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

class ActiveRuntimeState final {
public:
    static Result<std::unique_ptr<ActiveRuntimeState>> Create(
        RuntimeOptions options, std::uint64_t activation_generation);

    ActiveRuntimeState(const ActiveRuntimeState&) = delete;
    ActiveRuntimeState& operator=(const ActiveRuntimeState&) = delete;
    ActiveRuntimeState(ActiveRuntimeState&&) = delete;
    ActiveRuntimeState& operator=(ActiveRuntimeState&&) = delete;
    ~ActiveRuntimeState() = default;

    RuntimeOptions& options() noexcept { return options_; }
    const RuntimeOptions& options() const noexcept { return options_; }
    std::uint64_t activation_generation() const noexcept { return activation_generation_; }

    void SetAcquisitionControl(std::size_t plugin_index, rodaq_interface control);
    void SetStorageControl(rodaq_interface control);
    Result<rodaq_storage_capabilities> StorageCapabilities() const;

    Result<void> InitializePipeline(
        std::size_t effective_payload_limit, std::size_t maximum_payload_inflight_bytes,
        plugin::StoragePluginInstance* storage, std::chrono::nanoseconds progress_timeout);
    StoragePipeline* pipeline() noexcept { return pipeline_.get(); }
    std::optional<plugin::StoragePluginInstance>& storage_instance() noexcept {
        return storage_instance_;
    }
    std::optional<plugin::AcquisitionPluginInstance>& acquisition_instance(
        std::size_t index) noexcept {
        return acquisition_instances_[index];
    }

    Result<void> RegisterAcquisitionServices(
        plugin::ServiceRegistry& registry, std::size_t plugin_index);
    Result<void> RegisterStorageServices(plugin::ServiceRegistry& registry);

    void MarkRunning() noexcept;
    void MarkStopping() noexcept;
    void MarkStopped() noexcept;
    void MarkFailed() noexcept;
    RuntimeState state() const noexcept { return state_.load(); }
    bool GateOpen() const noexcept;

    bool BeginSubmission() noexcept;
    void EndSubmission() noexcept;
    bool WaitForAdmissionDrain(std::chrono::steady_clock::time_point deadline);
    bool WaitForSubmissions(std::chrono::steady_clock::time_point deadline);

    void RequestFailure(Error error) noexcept;
    Result<Error> PrimaryFailure() const;
    Result<std::vector<Error>> SecondaryFailures() const;
    std::uint64_t SecondaryFailureOverflowCount() const noexcept;
    RuntimeStatsSnapshot Stats() const noexcept { return stats_.Snapshot(); }
    void RecordPreRejected(bool gate_closed, bool timeout) noexcept;

    Result<rodaq_runtime_lifecycle_snapshot> LifecycleSnapshot() const;

private:
    friend struct AcquisitionServiceContext;
    friend struct StorageServiceContext;

    ActiveRuntimeState(RuntimeOptions options, std::uint64_t activation_generation);

public:
    Result<void> RegisterSource(
        std::size_t plugin_index, const rodaq_source_registration& request, SourceHandle& handle);
    Result<void> RegisterClock(
        std::size_t plugin_index, const rodaq_clock_registration& request, ClockHandle& handle);
    Result<void> RegisterSchema(const rodaq_schema_registration& request, SchemaHandle& handle);
    Result<void> RegisterStream(
        std::size_t plugin_index, const rodaq_stream_registration& request, StreamHandle& handle);
    Result<void> PublishStreamDescriptor(
        std::size_t plugin_index, const rodaq_stream_descriptor_registration& request,
        StreamDescriptorHandle& handle);
    Result<rodaq_acquisition_submit_result> Submit(
        std::size_t plugin_index, const rodaq_record_submission& submission,
        rodaq_payload_disposition& disposition);
    Result<void> ReportSourceAvailability(SourceHandle source, rodaq_availability_state state);
    Result<void> ReportStreamAvailability(StreamHandle stream, rodaq_availability_state state);

    Result<void> GetSource(SourceHandle handle, rodaq_source_view& view) const;
    Result<void> GetClock(ClockHandle handle, rodaq_clock_view& view) const;
    Result<void> GetSchema(SchemaHandle handle, rodaq_schema_view& view) const;
    Result<void> GetTimestampKeySet(std::uint64_t id, rodaq_timestamp_key_set_view& view) const;
    Result<void> GetStream(StreamHandle handle, rodaq_stream_view& view) const;
    Result<void> GetStreamDescriptor(
        StreamDescriptorHandle handle, rodaq_stream_descriptor_view& view) const;

    void RetireDrainingBinding(BindingHandle binding) noexcept;
    rodaq_interface AcquisitionServiceInterface(std::size_t plugin_index) noexcept;
    rodaq_interface StorageServiceInterface() noexcept;

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

    Result<DescriptorReadiness> EnsureDescriptorReady(StreamDescriptorHandle descriptor);
    Result<bool> ReconcileDescriptor(StreamDescriptorHandle descriptor);
    Result<void> ActivateDescriptorBinding(
        StreamDescriptorHandle descriptor, StreamHandle stream, std::size_t plugin_index,
        rodaq_interface control);
    void OnProjectionRegistered(
        rodaq_stream_descriptor_handle descriptor, std::string_view projection_id) noexcept;
    Result<BindingHandle> CreateBindingLocked(
        std::size_t plugin_index, StreamHandle stream, StreamDescriptorHandle descriptor,
        BindingLifecycle lifecycle);
    Result<void> InitializeIngressClock();

    RuntimeOptions options_;
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
    std::optional<plugin::StoragePluginInstance> storage_instance_;
    std::vector<std::optional<plugin::AcquisitionPluginInstance>> acquisition_instances_;

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
    std::vector<std::unique_ptr<AcquisitionServiceContext>> acquisition_contexts_;
    std::unique_ptr<StorageServiceContext> storage_context_;
    InteropProjectionRegistry projections_;
    ClockHandle ingress_clock_;
    std::optional<ClockCorrelation> ingress_clock_correlation_;

    std::atomic<RuntimeState> state_{RuntimeState::Constructed};
    mutable std::mutex admission_mutex_;
    std::condition_variable admission_condition_;
    bool gate_open_ = false;
    std::uint64_t active_submission_calls_ = 0;
    std::uint64_t admission_inflight_ = 0;

    mutable std::mutex failure_mutex_;
    std::optional<Error> primary_failure_;
    std::vector<Error> secondary_failures_;
    std::uint64_t secondary_overflow_count_ = 0;
    RuntimeStats stats_;
};

}  // namespace rodaq::runtime

#endif  // RODAQ_RUNTIME_ACTIVE_RUNTIME_STATE_HPP_
