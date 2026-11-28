#include "recording_instance.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <rodaq/acquisition/validation.hpp>
#include <rodaq/plugin/abi.hpp>

#include "../uuid.hpp"

namespace rodaq::pipeline {
namespace {

bool increment(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
        value = std::numeric_limits<std::uint64_t>::max();
        return false;
    }
    value += amount;
    return true;
}

}  // namespace

RecordingInstance::RecordingInstance(
    RecordingIdentity owner, time::PlaybackAnchor anchor, StreamTable stream_table,
    RecordBudget record_budget, PayloadPool pool, RecordQueue record_queue, InstanceLimits limits,
    DescriptorPreparation prepare, ReceiveClock sample, RecordNotification accepted)
: identity(owner),
  gate(owner.instance_id, owner.control_generation),
  streams(std::move(stream_table)),
  records(std::move(record_budget)),
  payloads(std::move(pool)),
  queue(std::move(record_queue)),
  limits_(limits),
  playback_anchor_(anchor),
  prepare_(std::move(prepare)),
  sample_(sample),
  accepted_(std::move(accepted)) {
    facts_.streams.resize(streams.bindings().size());
    for (std::size_t i = 0; i < streams.bindings().size(); ++i) {
        facts_.streams[i].stream_id = streams.bindings()[i].stream_id;
    }
}

Result<std::shared_ptr<RecordingInstance>> RecordingInstance::create(
    RecordingIdentity identity, std::vector<StreamPreparation> prepared, InstanceLimits limits,
    DescriptorPreparation prepare, ReceiveClock sample, RecordNotification accepted) {
    if (!uuid::nonzero(identity.task_id.value) || identity.pipeline_id == 0 ||
        !valid_instance_id(identity.instance_id) || identity.control_generation == 0 || !prepare ||
        sample == nullptr || limits.callback_concurrency == 0 ||
        limits.callback_concurrency > 1024 || limits.max_schema_bytes == 0 ||
        limits.max_offered_qos_profiles > 256) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto anchor_sample = sample();
    if (!anchor_sample.ok()) {
        return anchor_sample.status();
    }
    const time::PlaybackAnchor anchor{
        anchor_sample.value().monotonic_ns, anchor_sample.value().realtime_ns};
    try {
        for (const auto& stream : prepared) {
            if (stream.max_record_bytes > limits.payloads.max_payload_bytes ||
                stream.max_record_bytes > limits.logical_bytes) {
                return Status(StatusCode::InvalidArgument, {});
            }
        }
        auto table = StreamTable::create(std::move(prepared));
        if (!table.ok()) {
            return table.status();
        }
        auto records = RecordBudget::create(limits.records, limits.logical_bytes);
        if (!records.ok()) {
            return records.status();
        }
        auto payloads = PayloadPool::create(limits.payloads);
        if (!payloads.ok()) {
            return payloads.status();
        }
        auto queue = RecordQueue::create(limits.queue_capacity);
        if (!queue.ok()) {
            return queue.status();
        }
        return std::shared_ptr<RecordingInstance>(new RecordingInstance(
            identity, anchor, std::move(table.value()), std::move(records.value()),
            std::move(payloads.value()), std::move(queue.value()), limits, std::move(prepare),
            sample, std::move(accepted)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

acquisition::CallbacksTable RecordingInstance::callbacks_table(
    plugin::Deadline prepare_deadline) noexcept {
    prepare_deadline_ns_.store(
        prepare_deadline.native().monotonic_deadline, std::memory_order_release);
    acquisition::CallbacksTable table{};
    table.header.struct_size = sizeof(table);
    table.context = this;
    table.prepare_descriptor = &RecordingInstance::c_prepare_descriptor;
    table.submit_record = &RecordingInstance::c_submit_record;
    table.report_event = &RecordingInstance::c_report_event;
    return table;
}

rodaq_status_code_t RecordingInstance::c_prepare_descriptor(
    void* context, rodaq_stream_id_t stream_id, const rodaq_acquisition_descriptor_t* descriptor,
    rodaq_descriptor_id_t* descriptor_id, rodaq_error_t* error) noexcept {
    if (!context || !descriptor || !descriptor_id) {
        plugin::abi::write_error(error, "Invalid prepare_descriptor callback arguments");
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    try {
        auto* self = static_cast<RecordingInstance*>(context);
        const auto raw_deadline = self->prepare_deadline_ns_.load(std::memory_order_acquire);
        auto parsed_deadline = plugin::Deadline::from_native({raw_deadline});
        const auto deadline =
            parsed_deadline.ok() ? parsed_deadline.value() : plugin::Deadline::infinite();
        auto result = self->prepare_descriptor(stream_id, *descriptor, deadline);
        if (!result.ok()) {
            plugin::abi::write_error(error, result.status().message());
            return static_cast<rodaq_status_code_t>(result.status().code());
        }
        *descriptor_id = result.value();
        return RODAQ_STATUS_OK;
    } catch (const std::bad_alloc&) {
        plugin::abi::write_error(error, "Out of memory in prepare_descriptor");
        return RODAQ_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        plugin::abi::write_error(error, "Unexpected exception in prepare_descriptor");
        return RODAQ_STATUS_INTERNAL;
    }
}

rodaq_record_disposition_t RecordingInstance::c_submit_record(
    void* context, const rodaq_acquisition_record_attempt_t* record) noexcept {
    if (!context || !record) {
        return RODAQ_SUBMIT_INVALID_RECORD;
    }
    try {
        auto* self = static_cast<RecordingInstance*>(context);
        auto result = self->submit_record(*record);
        return result.ok()
                   ? result.value()
                   : static_cast<rodaq_record_disposition_t>(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    } catch (...) {
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
}

void RecordingInstance::c_report_event(
    void* context, const rodaq_acquisition_event_t* event) noexcept {
    if (!context || !event) {
        return;
    }
    try {
        auto* self = static_cast<RecordingInstance*>(context);
        (void)self->report_acquisition_event(*event);
    } catch (...) {
    }
}

RecordingInstance::Callback::Callback(Callback&& other) noexcept
: instance_(std::exchange(other.instance_, nullptr)) {}

RecordingInstance::Callback::~Callback() noexcept {
    if (instance_) {
        std::lock_guard<std::mutex> lock(instance_->facts_mutex_);
        --instance_->facts_.callbacks;
    }
}

Result<RecordingInstance::Callback> RecordingInstance::enter_callback() {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    if (facts_.callbacks == limits_.callback_concurrency) {
        increment(facts_.callback_rejections);
        facts_.evidence_complete = false;
        readiness_changed_.notify_all();
        return Status(StatusCode::ResourceExhausted, {});
    }
    ++facts_.callbacks;
    facts_.high_water_callbacks = std::max(facts_.high_water_callbacks, facts_.callbacks);
    return Callback(*this);
}

std::size_t RecordingInstance::stream_index(StreamId stream) const noexcept {
    const auto& bindings = streams.bindings();
    const auto found = std::lower_bound(
        bindings.begin(), bindings.end(), stream,
        [](const auto& entry, StreamId id) { return entry.stream_id < id; });
    return found != bindings.end() && found->stream_id == stream
               ? static_cast<std::size_t>(found - bindings.begin())
               : bindings.size();
}

void RecordingInstance::submission(std::size_t index, rodaq_record_disposition_t disposition) {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    if (disposition < facts_.streams[index].submissions.size()) {
        increment(facts_.streams[index].submissions[disposition]);
    }
}

Result<void> RecordingInstance::freeze_open_timeline(std::uint64_t opened, std::uint64_t base) {
    std::lock_guard<std::mutex> lock(timeline_mutex_);
    if (timeline_frozen_.load(std::memory_order_acquire) || gate.admission_fenced() ||
        gate.snapshot().open || opened < playback_anchor_.monotonic_ns) {
        return Status(StatusCode::InvalidState, {});
    }
    opened_monotonic_ = opened;
    recording_base_ = base;
    timeline_frozen_.store(true, std::memory_order_release);
    return {};
}

Result<rodaq_descriptor_id_t> RecordingInstance::prepare_descriptor(
    StreamId stream_id, const acquisition::Descriptor& proposal, plugin::Deadline deadline) {
    auto callback = enter_callback();
    if (!callback.ok()) {
        return callback.status();
    }
    if (stream_id == 0 || proposal.stream_id != stream_id) {
        return Status(StatusCode::InvalidArgument, {});
    }
    // Bind-once during Prepare only: reject after Open or control fence.
    if (gate.ever_opened() || gate.admission_fenced()) {
        return Status(StatusCode::InvalidState, {});
    }
    auto* stream = streams.find(stream_id);
    if (!stream) {
        return Status(StatusCode::NotFound, {});
    }
    std::shared_ptr<RecordingSegment> segment;
    {
        std::lock_guard<std::mutex> lock(stream->mutex);
        if (stream->active_descriptor) {
            // Bind-once: runtime/duplicate re-bind is rejected fail-closed.
            return Status(StatusCode::Conflict, {});
        }
        if (!stream->segment) {
            return Status(StatusCode::InvalidState, {});
        }
        segment = stream->segment;
    }
    const rodaq_descriptor_id_t descriptor_id = 1;
    auto created = Descriptor::create(
        proposal, descriptor_id, limits_.max_schema_bytes, limits_.max_offered_qos_profiles);
    if (!created.ok()) {
        return created.status();
    }
    auto next = std::move(created.value());
    auto prepared = prepare_(segment, next, deadline);
    if (!prepared.ok()) {
        if (segment->capacity_snapshot().admission_failure != StatusCode::Ok) {
            (void)establish_control_fence();
        }
        return prepared.status();
    }
    {
        std::lock_guard<std::mutex> lock(stream->mutex);
        if (stream->active_descriptor || stream->segment != segment || gate.ever_opened() ||
            gate.admission_fenced()) {
            return Status(StatusCode::Conflict, {});
        }
        stream->active_descriptor = next;
    }
    {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        facts_.streams[stream_index(stream_id)].descriptor_ready = true;
        readiness_changed_.notify_all();
    }
    return descriptor_id;
}

Result<acquisition::RecordDisposition> RecordingInstance::submit_record(
    const acquisition::RecordAttempt& attempt) {
    const auto index = stream_index(attempt.stream_id);
    if (index != streams.bindings().size()) {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        auto& stream = facts_.streams[index];
        const bool records_complete = increment(stream.received_records);
        const bool bytes_complete = increment(stream.received_logical_bytes, attempt.payload.size);
        stream.evidence_complete = stream.evidence_complete && records_complete && bytes_complete;
        facts_.evidence_complete = facts_.evidence_complete && stream.evidence_complete;
    }
    auto callback = enter_callback();
    if (!callback.ok()) {
        if (index != streams.bindings().size()) {
            submission(index, RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
        }
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    if (index == streams.bindings().size()) {
        return RODAQ_SUBMIT_INVALID_RECORD;
    }
    if (!timeline_frozen_.load(std::memory_order_acquire) || gate.admission_fenced()) {
        submission(index, RODAQ_SUBMIT_GATE_CLOSED);
        return RODAQ_SUBMIT_GATE_CLOSED;
    }
    auto sample = sample_();
    if (!sample.ok()) {
        (void)establish_control_fence();
        std::lock_guard<std::mutex> lock(facts_mutex_);
        increment(facts_.clock_rejections);
        increment(facts_.streams[index].submissions[RODAQ_SUBMIT_RESOURCE_EXHAUSTED]);
        facts_.terminal_failure = true;
        return RODAQ_SUBMIT_RESOURCE_EXHAUSTED;
    }
    {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        facts_.streams[index].last_receive_realtime_ns = sample.value().realtime_ns;
    }
    AdmissionDomain domain{
        identity.task_id,
        identity.pipeline_id,
        identity.instance_id,
        identity.control_generation,
        gate,
        records,
        payloads,
        queue,
        playback_anchor_,
        recording_base_,
        opened_monotonic_};
    auto submitted =
        pipeline::submit_record(domain, *streams.find(attempt.stream_id), attempt, sample.value());
    if (gate.admission_fenced()) {
        (void)establish_control_fence(sample.value().monotonic_ns);
    }
    if (submitted.ok()) {
        submission(index, submitted.value());
    }
    if (submitted.ok() && submitted.value() == RODAQ_SUBMIT_ACCEPTED) {
        const auto recorded = recording_base_ + (sample.value().monotonic_ns - opened_monotonic_);
        auto old = accepted_recording_high_water_.load(std::memory_order_relaxed);
        while (old < recorded && !accepted_recording_high_water_.compare_exchange_weak(
                                     old, recorded, std::memory_order_relaxed)) {
        }
    }
    if (submitted.ok() && submitted.value() == RODAQ_SUBMIT_ACCEPTED && accepted_) {
        try {
            accepted_();
        } catch (...) {
            (void)establish_control_fence(sample.value().monotonic_ns);
            std::lock_guard<std::mutex> lock(facts_mutex_);
            facts_.terminal_failure = true;
        }
    }
    return submitted;
}

Result<void> RecordingInstance::report_acquisition_event(
    const acquisition::AcquisitionEvent& event) {
    auto callback = enter_callback();
    if (!callback.ok()) {
        return callback.status();
    }
    if (!acquisition::abi::valid_acquisition_event(&event)) {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        readiness_changed_.notify_all();
        increment(facts_.invalid_events);
        facts_.evidence_complete = false;
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto index = event.has_stream_id ? stream_index(event.stream_id) : facts_.streams.size();
    {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        readiness_changed_.notify_all();
        if (event.has_stream_id && index == facts_.streams.size()) {
            increment(facts_.invalid_events);
            facts_.evidence_complete = false;
            return Status(StatusCode::InvalidArgument, {});
        }
        if (event.has_stream_id) {
            auto& facts = facts_.streams[index];
            increment(facts.acquisition_events[event.kind]);
            if (!increment(facts.reported_counts[event.kind], event.count)) {
                facts.evidence_complete = false;
                facts_.evidence_complete = false;
            }
            if (event.kind == RODAQ_ACQUISITION_EVENT_STREAM_READY) {
                facts.stream_ready = true;
            } else if (event.kind == RODAQ_ACQUISITION_EVENT_STREAM_UNAVAILABLE) {
                facts.stream_ready = false;
            }
        }
        if (event.kind == RODAQ_ACQUISITION_EVENT_TRANSPORT_READY) {
            facts_.transport_ready = true;
        } else if (event.kind == RODAQ_ACQUISITION_EVENT_TRANSPORT_LOST) {
            facts_.transport_ready = false;
        } else if (event.kind == RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE) {
            facts_.terminal_failure = true;
        }
    }
    if (event.kind == RODAQ_ACQUISITION_EVENT_TERMINAL_FAILURE) {
        const auto sample = sample_();
        (void)establish_control_fence(
            sample.ok() ? std::optional<std::uint64_t>(sample.value().monotonic_ns) : std::nullopt);
    }
    return {};
}

Result<CallbackFacts> RecordingInstance::callback_facts() const {
    try {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        return facts_;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

bool RecordingInstance::ready_locked(const std::vector<ReadinessPolicy>& policies) const noexcept {
    if (!facts_.transport_ready || facts_.terminal_failure || !facts_.evidence_complete ||
        policies.size() != facts_.streams.size()) {
        return false;
    }
    for (std::size_t i = 0; i < policies.size(); ++i) {
        const auto& stream = facts_.streams[i];
        if (!stream.descriptor_ready || !stream.evidence_complete) {
            return false;
        }
        if (policies[i] == ReadinessPolicy::SourceReady) {
            if (!stream.stream_ready) {
                return false;
            }
        } else if (policies[i] != ReadinessPolicy::EntryReady) {
            return false;
        }
    }
    return true;
}

bool RecordingInstance::ready(const std::vector<ReadinessPolicy>& policies) const {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    return ready_locked(policies);
}

Result<void> RecordingInstance::wait_ready(
    const std::vector<ReadinessPolicy>& policies, plugin::Deadline deadline) {
    std::unique_lock<std::mutex> lock(facts_mutex_);
    const auto terminal = [&] {
        return gate.admission_fenced() || facts_.terminal_failure || !facts_.evidence_complete ||
               ready_locked(policies);
    };
    if (deadline.is_infinite()) {
        readiness_changed_.wait(lock, terminal);
    } else {
        auto remaining = deadline.remaining();
        if (!remaining.ok()) {
            return remaining.status();
        }
        if (!readiness_changed_.wait_for(lock, remaining.value(), terminal)) {
            return Status(StatusCode::DeadlineExceeded, {});
        }
    }
    return !gate.admission_fenced() && ready_locked(policies)
               ? Result<void>{}
               : Result<void>(Status(StatusCode::InvalidState, {}));
}

Result<void> RecordingInstance::establish_control_fence(
    std::optional<std::uint64_t> monotonic_ns) noexcept {
    Result<void> result;
    {
        std::lock_guard<std::mutex> timeline(timeline_mutex_);
        const bool fenced = gate.admission_fenced();
        result = gate.establish_control_fence(identity.instance_id);
        if (!fenced && result.ok() && timeline_frozen_.load(std::memory_order_acquire) &&
            gate.ever_opened()) {
            fenced_monotonic_ = monotonic_ns;
        }
    }
    for (const auto& binding : streams.bindings()) {
        auto* stream = streams.find(binding.stream_id);
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->segment_changed.notify_all();
    }
    std::lock_guard<std::mutex> lock(facts_mutex_);
    readiness_changed_.notify_all();
    return result;
}

Result<std::optional<std::uint64_t>> RecordingInstance::recording_time_boundary() const {
    std::lock_guard<std::mutex> timeline(timeline_mutex_);
    const auto state = gate.snapshot();
    if (!state.ever_opened) {
        return std::optional<std::uint64_t>{};
    }
    if (!state.control_fence) {
        return Status(StatusCode::Unavailable, {});
    }
    if (fenced_monotonic_ && *fenced_monotonic_ < opened_monotonic_) {
        return Status(StatusCode::DataLoss, {});
    }
    const auto elapsed = fenced_monotonic_ ? *fenced_monotonic_ - opened_monotonic_ : 0;
    if (elapsed > std::numeric_limits<std::uint64_t>::max() - recording_base_) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    return std::optional<std::uint64_t>{std::max(
        recording_base_ + elapsed, accepted_recording_high_water_.load(std::memory_order_relaxed))};
}

}  // namespace rodaq::pipeline
