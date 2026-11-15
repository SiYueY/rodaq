#include "recording_instance.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <rodaq/acquisition/validation.hpp>

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

acquisition::SubmitResult rejected(rodaq_submit_disposition_t disposition) noexcept {
    return {{sizeof(acquisition::SubmitResult)}, disposition, 0};
}

bool same_source(const rodaq_source_id_t& left, const rodaq_source_id_t& right) noexcept {
    return left.kind == right.kind && left.size == right.size &&
           std::equal(std::begin(left.data), std::end(left.data), std::begin(right.data));
}

// A proposal is single-flight for one Stream through actual preparation, not
// through an observer wait. Destruction happens after any Gate/Stream cut guard.
class Proposal {
public:
    explicit Proposal(StreamAdmission& stream) noexcept : stream_(stream) {}
    ~Proposal() noexcept {
        std::lock_guard<std::mutex> lock(stream_.mutex);
        stream_.descriptor_proposal_inflight = false;
    }
    Proposal(const Proposal&) = delete;
    Proposal& operator=(const Proposal&) = delete;

private:
    StreamAdmission& stream_;
};

}  // namespace

RecordingInstance::RecordingInstance(
    RecordingIdentity owner, StreamTable stream_table, DescriptorRegistry registry,
    RecordBudget record_budget, PayloadPool pool, RecordQueue record_queue, InstanceLimits limits,
    std::shared_ptr<time::ClockCorrelation> clock, DescriptorPreparation prepare,
    ReceiveClock sample)
: identity(owner),
  gate(owner.instance_id, owner.control_generation),
  streams(std::move(stream_table)),
  descriptors(std::move(registry)),
  records(std::move(record_budget)),
  payloads(std::move(pool)),
  queue(std::move(record_queue)),
  limits_(limits),
  clock_(std::move(clock)),
  prepare_(std::move(prepare)),
  sample_(sample) {
    facts_.streams.resize(streams.bindings().size());
    sources_.resize(streams.bindings().size());
    for (std::size_t i = 0; i < streams.bindings().size(); ++i) {
        facts_.streams[i].stream_id = streams.bindings()[i].stream_id;
        sources_[i].resize(limits_.sources_per_stream);
    }
}

Result<std::shared_ptr<RecordingInstance>> RecordingInstance::create(
    RecordingIdentity identity, std::vector<StreamPreparation> prepared, InstanceLimits limits,
    std::shared_ptr<time::ClockCorrelation> clock, DescriptorPreparation prepare,
    ReceiveClock sample) {
    if (!uuid::nonzero(identity.task_id.value) || identity.pipeline_id == 0 ||
        !valid_instance_id(identity.instance_id) || identity.control_generation == 0 || !clock ||
        !prepare || sample == nullptr || limits.callback_concurrency == 0 ||
        limits.callback_concurrency > 1024 || limits.sources_per_stream == 0 ||
        limits.sources_per_stream > 256) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto time_ready = clock->snapshot();
    if (!time_ready.ok()) {
        return time_ready.status();
    }
    try {
        std::vector<StreamId> identities;
        identities.reserve(prepared.size());
        for (const auto& stream : prepared) {
            if (stream.max_record_bytes > limits.payloads.max_payload_bytes ||
                stream.max_record_bytes > limits.logical_bytes) {
                return Status(StatusCode::InvalidArgument, {});
            }
            identities.push_back(stream.stream_id);
        }
        auto table = StreamTable::create(std::move(prepared));
        if (!table.ok()) {
            return table.status();
        }
        auto registry = DescriptorRegistry::create(std::move(identities), limits.descriptors);
        if (!registry.ok()) {
            return registry.status();
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
            identity, std::move(table.value()), std::move(registry.value()),
            std::move(records.value()), std::move(payloads.value()), std::move(queue.value()),
            limits, std::move(clock), std::move(prepare), sample));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
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

void RecordingInstance::submission(std::size_t index, rodaq_submit_disposition_t disposition) {
    std::lock_guard<std::mutex> lock(facts_mutex_);
    increment(facts_.streams[index].submissions[disposition]);
}

Result<void> RecordingInstance::freeze_open_timeline(std::uint64_t opened, std::uint64_t base) {
    std::lock_guard<std::mutex> lock(timeline_mutex_);
    if (timeline_frozen_.load(std::memory_order_acquire) || gate.admission_fenced() ||
        gate.snapshot().open || opened < clock_->anchor().monotonic_ns) {
        return Status(StatusCode::InvalidState, {});
    }
    opened_monotonic_ = opened;
    recording_base_ = base;
    timeline_frozen_.store(true, std::memory_order_release);
    return {};
}

Result<acquisition::SubmitResult> RecordingInstance::submit_record(
    const acquisition::RecordAttempt& attempt) {
    auto callback = enter_callback();
    if (!callback.ok()) {
        const auto index = stream_index(attempt.stream_id);
        if (index != streams.bindings().size()) {
            submission(index, RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
        }
        return rejected(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    }
    const auto index = stream_index(attempt.stream_id);
    if (index == streams.bindings().size()) {
        return rejected(RODAQ_SUBMIT_INVALID_RECORD);
    }
    if (!timeline_frozen_.load(std::memory_order_acquire) || gate.admission_fenced()) {
        submission(index, RODAQ_SUBMIT_GATE_CLOSED);
        return rejected(RODAQ_SUBMIT_GATE_CLOSED);
    }
    // Fresh paired receive clocks precede COPY, Gate and sequence allocation.
    auto sample = sample_();
    auto received = sample.ok() ? clock_->capture_receive(sample.value())
                                : Result<time::ClockSnapshot>{sample.status()};
    if (!received.ok()) {
        // An unobserved jump may recover through the observation lane. A
        // failed sampler/authority cannot keep advertising a recording Gate.
        const bool authority_failed = !sample.ok() || !clock_->snapshot().ok();
        if (authority_failed) {
            (void)gate.establish_control_fence(identity.instance_id);
        }
        std::lock_guard<std::mutex> lock(facts_mutex_);
        increment(facts_.clock_rejections);
        increment(facts_.streams[index].submissions[RODAQ_SUBMIT_RESOURCE_EXHAUSTED]);
        facts_.terminal_failure = facts_.terminal_failure || authority_failed;
        return rejected(RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
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
        recording_base_,
        opened_monotonic_};
    auto submitted = pipeline::submit_record(
        domain, *streams.find(attempt.stream_id), attempt, received.value());
    if (submitted.ok()) {
        submission(index, submitted.value().disposition);
        if (submitted.value().disposition == RODAQ_SUBMIT_ACCEPTED && attempt.has_source_id) {
            std::lock_guard<std::mutex> lock(facts_mutex_);
            observe_source(index, attempt.source_id, std::nullopt);
        }
    }
    return submitted;
}

Result<acquisition::DescriptorToken> RecordingInstance::propose_descriptor(
    const acquisition::DescriptorProposal& proposal, plugin::Deadline deadline) {
    auto callback = enter_callback();
    if (!callback.ok()) {
        return callback.status();
    }
    if (proposal.schema.size > limits_.descriptors.max_schema_bytes ||
        proposal.ros2_offered_qos_profile_count > limits_.descriptors.max_offered_qos_profiles ||
        !acquisition::abi::valid_descriptor(&proposal)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (gate.admission_fenced()) {
        return Status(StatusCode::InvalidState, {});
    }
    auto* stream = streams.find(proposal.stream_id);
    if (!stream) {
        return Status(StatusCode::NotFound, {});
    }
    DescriptorRef previous;
    std::shared_ptr<RecordingSegment> segment;
    {
        std::lock_guard<std::mutex> lock(stream->mutex);
        if (stream->descriptor_proposal_inflight || !stream->segment) {
            return Status(StatusCode::Conflict, {});
        }
        stream->descriptor_proposal_inflight = true;
        previous = stream->active_descriptor;
        segment = stream->segment;
    }
    Proposal flight(*stream);
    bool unchanged = false;
    if (previous) {
        auto equivalent = descriptors.equivalent(*previous, proposal);
        if (!equivalent.ok()) {
            return equivalent.status();
        }
        unchanged = equivalent.value();
    }
    DescriptorRef next = previous;
    if (!unchanged) {
        auto id = [&] {
            std::lock_guard<std::mutex> lock(stream->mutex);
            return stream->descriptor_ids.allocate();
        }();
        if (!id.ok()) {
            return id.status();
        }
        auto captured = descriptors.capture(proposal, id.value());
        if (!captured.ok()) {
            return captured.status();
        }
        next = std::move(captured.value());
        auto sample = sample_();
        if (!sample.ok()) {
            return sample.status();
        }
        auto prepared = prepare_(segment, next, sample.value().monotonic_ns, deadline);
        if (!prepared.ok()) {
            return prepared.status();
        }
    }
    {
        auto cut = gate.enter_descriptor_cut(identity.instance_id, identity.control_generation);
        if (!cut.ok()) {
            return cut.status();
        }
        std::lock_guard<std::mutex> lock(stream->mutex);
        if (stream->active_descriptor != previous || stream->segment != segment) {
            return Status(StatusCode::Conflict, {});
        }
        stream->active_descriptor = next;
    }
    {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        facts_.streams[stream_index(proposal.stream_id)].descriptor_ready = true;
    }
    return next->token;
}

void RecordingInstance::observe_source(
    std::size_t index, const rodaq_source_id_t& id, std::optional<bool> visible) {
    auto& slots = sources_[index];
    auto& facts = facts_.streams[index];
    auto found = std::find_if(slots.begin(), slots.end(), [&](const auto& slot) {
        return slot.occupied && same_source(slot.id, id);
    });
    if (found == slots.end()) {
        found = std::find_if(slots.begin(), slots.end(), [](const auto& slot) {
            return !slot.occupied || !slot.visible;
        });
        if (found == slots.end()) {
            increment(facts.source_overflow);
            facts.evidence_complete = false;
            facts_.evidence_complete = false;
            return;
        }
        *found = Source{id, true, false};
        if (!visible) {
            facts.evidence_complete = false;
            facts_.evidence_complete = false;
        }
    }
    // A late Record does not resurrect explicit lost/ended native evidence.
    if (visible && found->visible != *visible) {
        if (*visible) {
            ++facts.active_sources;
        } else {
            --facts.active_sources;
        }
        found->visible = *visible;
    }
}

Result<void> RecordingInstance::report_source_event(const acquisition::SourceEvent& event) {
    auto callback = enter_callback();
    if (!callback.ok()) {
        return callback.status();
    }
    const auto index = stream_index(event.stream_id);
    std::lock_guard<std::mutex> lock(facts_mutex_);
    if (index == facts_.streams.size() || !acquisition::abi::valid_source_event(&event)) {
        increment(facts_.invalid_events);
        facts_.evidence_complete = false;
        return Status(StatusCode::InvalidArgument, {});
    }
    auto& facts = facts_.streams[index];
    increment(facts.source_events[event.kind]);
    if (event.has_source_id) {
        std::optional<bool> visible;
        if (event.kind == RODAQ_SOURCE_EVENT_DISCOVERED ||
            event.kind == RODAQ_SOURCE_EVENT_SESSION_STARTED) {
            visible = true;
        } else if (
            event.kind == RODAQ_SOURCE_EVENT_LOST ||
            event.kind == RODAQ_SOURCE_EVENT_SESSION_ENDED) {
            visible = false;
        }
        observe_source(index, event.source_id, visible);
    } else {
        facts.evidence_complete = false;
        facts_.evidence_complete = false;
    }
    return {};
}

Result<void> RecordingInstance::report_acquisition_event(
    const acquisition::AcquisitionEvent& event) {
    auto callback = enter_callback();
    if (!callback.ok()) {
        return callback.status();
    }
    if (!acquisition::abi::valid_acquisition_event(&event)) {
        std::lock_guard<std::mutex> lock(facts_mutex_);
        increment(facts_.invalid_events);
        facts_.evidence_complete = false;
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto index = event.has_stream_id ? stream_index(event.stream_id) : facts_.streams.size();
    {
        std::lock_guard<std::mutex> lock(facts_mutex_);
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
        (void)gate.establish_control_fence(identity.instance_id);
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

}  // namespace rodaq::pipeline
