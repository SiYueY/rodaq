#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <optional>
#include <rodaq/plugin/deadline.hpp>
#include <memory>
#include <mutex>
#include <vector>

#include "stream_table.hpp"
#include "descriptor_registry.hpp"
#include "../time/clock_correlation.hpp"

namespace rodaq::pipeline {

struct RecordingIdentity {
    TaskId task_id;
    PipelineId pipeline_id{0};
    InstanceId instance_id;
    ControlGeneration control_generation{0};
};

struct InstanceLimits {
    std::uint32_t records{256};
    std::uint64_t logical_bytes{1024 * 1024};
    PayloadPoolOptions payloads;
    std::size_t queue_capacity{256};
    std::uint32_t callback_concurrency{4};
    std::uint32_t sources_per_stream{64};
    DescriptorRegistryOptions descriptors;
};

struct StreamCallbackFacts {
    StreamId stream_id{0};
    std::uint64_t received_records{0};
    std::uint64_t received_logical_bytes{0};
    std::optional<std::int64_t> last_receive_realtime_ns;
    std::array<std::uint64_t, 9> submissions{};
    std::array<std::uint64_t, 7> source_events{};
    std::array<std::uint64_t, 11> acquisition_events{};
    std::array<std::uint64_t, 11> reported_counts{};
    std::uint64_t active_sources{0};
    std::uint64_t source_overflow{0};
    bool descriptor_ready{false};
    bool stream_ready{false};
    bool evidence_complete{true};
};

struct CallbackFacts {
    std::vector<StreamCallbackFacts> streams;
    std::uint64_t clock_rejections{0};
    std::uint64_t invalid_events{0};
    std::uint64_t callback_rejections{0};
    // Host method bodies only: full quiescence also requires actual Acquisition
    // quiesce, Writer/prepare/Commit frames and accepted-work settlement.
    std::uint32_t callbacks{0};
    std::uint32_t high_water_callbacks{0};
    bool evidence_complete{true};
    bool transport_ready{false};
    bool terminal_failure{false};
};

// Execution mapping supplied by Pipeline's bounded Writer lane. Returns only
// after the actual operation ends, even if an observer's deadline expired.
// It must not return an observation timeout while Native work still runs.
// The actual work frame retains Instance/Segment/Descriptor; no callbacks or
// waits run under Gate, Stream or facts locks. Native inputs are Core copies.
using DescriptorPreparation = std::function<Result<void>(
    std::shared_ptr<RecordingSegment>, DescriptorRef, std::uint64_t, plugin::Deadline)>;
using ReceiveClock = Result<time::ClockSample> (*)();
using RecordNotification = std::function<void()>;

// Actual per-Pipeline Recording Instance backing and Acquisition Service owner.
// Registry/queued/Native work retain shared ownership through actual return.
// Task intent, durable OpenAuthorization, scheduling and Artifact work remain
// with their owners. This object never creates threads or opens its own Gate.
class RecordingInstance {
public:
    static Result<std::shared_ptr<RecordingInstance>> create(
        RecordingIdentity identity, std::vector<StreamPreparation> streams, InstanceLimits limits,
        std::shared_ptr<time::ClockCorrelation> clock, DescriptorPreparation prepare_descriptor,
        ReceiveClock receive_clock = &time::sample_clocks, RecordNotification accepted = {});

    // Task's control owner calls once while closed, after its durable full-set
    // OpenAuthorization, immediately before opening Gate. Publication is only
    // timeline data; it never manufactures that durable authority or opens Gate.
    Result<void> freeze_open_timeline(std::uint64_t opened_monotonic, std::uint64_t recording_base);
    Result<acquisition::SubmitResult> submit_record(const acquisition::RecordAttempt& attempt);
    Result<acquisition::DescriptorToken> propose_descriptor(
        const acquisition::DescriptorProposal& proposal, plugin::Deadline deadline);
    Result<void> report_source_event(const acquisition::SourceEvent& event);
    Result<void> report_acquisition_event(const acquisition::AcquisitionEvent& event);
    Result<CallbackFacts> callback_facts() const;
    // Frozen policy rows align with StreamTable bindings; no allocation or I/O.
    bool ready(const std::vector<ReadinessPolicy>& policies) const;
    Result<void> wait_ready(
        const std::vector<ReadinessPolicy>& policies, plugin::Deadline deadline);
    Result<void> establish_control_fence(std::optional<std::uint64_t> monotonic_ns = {}) noexcept;
    Result<std::optional<std::uint64_t>> recording_time_boundary() const;

    const RecordingIdentity identity;
    AdmissionGate gate;
    StreamTable streams;
    DescriptorRegistry descriptors;
    RecordBudget records;
    PayloadPool payloads;
    RecordQueue queue;

private:
    std::optional<std::uint64_t> fenced_monotonic_;
    std::atomic<std::uint64_t> accepted_recording_high_water_{0};
    struct Source {
        rodaq_source_id_t id{};
        bool occupied{false};
        bool visible{false};
    };
    class Callback {
    public:
        explicit Callback(RecordingInstance& instance) noexcept : instance_(&instance) {}
        Callback(const Callback&) = delete;
        Callback& operator=(const Callback&) = delete;
        Callback(Callback&& other) noexcept;
        ~Callback() noexcept;

    private:
        RecordingInstance* instance_;
    };
    RecordingInstance(
        RecordingIdentity identity, StreamTable streams, DescriptorRegistry descriptors,
        RecordBudget records, PayloadPool payloads, RecordQueue queue, InstanceLimits limits,
        std::shared_ptr<time::ClockCorrelation> clock, DescriptorPreparation prepare,
        ReceiveClock sample, RecordNotification accepted);
    Result<Callback> enter_callback();
    std::size_t stream_index(StreamId stream) const noexcept;
    void submission(std::size_t index, rodaq_submit_disposition_t disposition);
    void observe_source(
        std::size_t index, const rodaq_source_id_t& source, std::optional<bool> visible);
    bool ready_locked(const std::vector<ReadinessPolicy>& policies) const noexcept;

    const InstanceLimits limits_;
    std::shared_ptr<time::ClockCorrelation> clock_;
    DescriptorPreparation prepare_;
    const ReceiveClock sample_;
    const RecordNotification accepted_;
    mutable std::mutex facts_mutex_;
    std::condition_variable readiness_changed_;
    CallbackFacts facts_;
    std::vector<std::vector<Source>> sources_;
    std::atomic<bool> timeline_frozen_{false};
    mutable std::mutex timeline_mutex_;
    std::uint64_t opened_monotonic_{0};
    std::uint64_t recording_base_{0};
};

}  // namespace rodaq::pipeline
