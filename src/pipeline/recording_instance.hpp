#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <rodaq/plugin/deadline.hpp>

#include "../time/clock.hpp"
#include "stream_table.hpp"

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
    std::uint32_t max_schema_bytes{1024 * 1024};
    std::uint32_t max_offered_qos_profiles{256};
};

struct StreamCallbackFacts {
    StreamId stream_id{0};
    std::uint64_t received_records{0};
    std::uint64_t received_logical_bytes{0};
    std::optional<std::int64_t> last_receive_realtime_ns;
    std::array<std::uint64_t, 9> submissions{};
    std::array<std::uint64_t, 11> acquisition_events{};
    std::array<std::uint64_t, 11> reported_counts{};
    bool descriptor_ready{false};
    bool stream_ready{false};
    bool evidence_complete{true};
};

struct CallbackFacts {
    std::vector<StreamCallbackFacts> streams;
    std::uint64_t clock_rejections{0};
    std::uint64_t invalid_events{0};
    std::uint64_t callback_rejections{0};
    std::uint32_t callbacks{0};
    std::uint32_t high_water_callbacks{0};
    bool evidence_complete{true};
    bool transport_ready{false};
    bool terminal_failure{false};
};

using DescriptorPreparation =
    std::function<Result<void>(std::shared_ptr<RecordingSegment>, DescriptorRef, plugin::Deadline)>;
using ReceiveClock = Result<time::ClockSample> (*)();
using RecordNotification = std::function<void()>;

// Per-Pipeline Recording Instance backing and Acquisition callback target.
// Holds one per-instance PlaybackAnchor and bind-once Stream Descriptors.
class RecordingInstance {
public:
    static Result<std::shared_ptr<RecordingInstance>> create(
        RecordingIdentity identity, std::vector<StreamPreparation> streams, InstanceLimits limits,
        DescriptorPreparation prepare_descriptor, ReceiveClock receive_clock = &time::sample_clocks,
        RecordNotification accepted = {});

    time::PlaybackAnchor playback_anchor() const noexcept { return playback_anchor_; }

    // Builds the C callback table passed to Acquisition::prepare(). The caller
    // must keep this RecordingInstance alive until Acquisition::quiesce() ends.
    acquisition::CallbacksTable callbacks_table(plugin::Deadline prepare_deadline) noexcept;

    Result<void> freeze_open_timeline(std::uint64_t opened_monotonic, std::uint64_t recording_base);
    Result<rodaq_descriptor_id_t> prepare_descriptor(
        StreamId stream_id, const acquisition::Descriptor& descriptor, plugin::Deadline deadline);
    Result<acquisition::RecordDisposition> submit_record(const acquisition::RecordAttempt& attempt);
    Result<void> report_acquisition_event(const acquisition::AcquisitionEvent& event);
    Result<CallbackFacts> callback_facts() const;
    bool ready(const std::vector<ReadinessPolicy>& policies) const;
    Result<void> wait_ready(
        const std::vector<ReadinessPolicy>& policies, plugin::Deadline deadline);
    Result<void> establish_control_fence(std::optional<std::uint64_t> monotonic_ns = {}) noexcept;
    Result<std::optional<std::uint64_t>> recording_time_boundary() const;

    const RecordingIdentity identity;
    AdmissionGate gate;
    StreamTable streams;
    RecordBudget records;
    PayloadPool payloads;
    RecordQueue queue;

private:
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
        RecordingIdentity identity, time::PlaybackAnchor anchor, StreamTable streams,
        RecordBudget records, PayloadPool payloads, RecordQueue queue, InstanceLimits limits,
        DescriptorPreparation prepare, ReceiveClock sample, RecordNotification accepted);

    static rodaq_status_code_t c_prepare_descriptor(
        void* context, rodaq_stream_id_t stream_id,
        const rodaq_acquisition_descriptor_t* descriptor, rodaq_descriptor_id_t* descriptor_id,
        rodaq_error_t* error) noexcept;
    static rodaq_record_disposition_t c_submit_record(
        void* context, const rodaq_acquisition_record_attempt_t* record) noexcept;
    static void c_report_event(void* context, const rodaq_acquisition_event_t* event) noexcept;

    Result<Callback> enter_callback();
    std::size_t stream_index(StreamId stream) const noexcept;
    void submission(std::size_t index, rodaq_record_disposition_t disposition);
    bool ready_locked(const std::vector<ReadinessPolicy>& policies) const noexcept;

    const InstanceLimits limits_;
    const time::PlaybackAnchor playback_anchor_;
    DescriptorPreparation prepare_;
    const ReceiveClock sample_;
    const RecordNotification accepted_;
    std::atomic<std::int64_t> prepare_deadline_ns_{RODAQ_DEADLINE_INFINITE};
    mutable std::mutex facts_mutex_;
    std::condition_variable readiness_changed_;
    CallbackFacts facts_;
    std::atomic<bool> timeline_frozen_{false};
    mutable std::mutex timeline_mutex_;
    std::uint64_t opened_monotonic_{0};
    std::uint64_t recording_base_{0};
    std::optional<std::uint64_t> fenced_monotonic_;
    std::atomic<std::uint64_t> accepted_recording_high_water_{0};
};

}  // namespace rodaq::pipeline
