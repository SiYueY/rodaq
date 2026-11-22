#include "pipeline_controller.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "pipeline_controller_state.hpp"
#include "../work_id.hpp"

namespace rodaq::pipeline {
namespace {

template <typename T>
bool add(T& target, T value) noexcept {
    if (value > std::numeric_limits<T>::max() - target) {
        return false;
    }
    target += value;
    return true;
}

Result<void> abort_unopened(
    const std::shared_ptr<RecordingSegment>& segment,
    const std::shared_ptr<artifact::ManagedVolume>& volume) {
    segment->close_admission();
    auto verified = volume->verify();
    if (!verified.ok()) {
        return verified.status();
    }
    std::error_code error;
    const auto actual = std::filesystem::symlink_status(
        volume->artifacts_directory() / (std::to_string(segment->metadata.segment_id()) + ".tmp"),
        error);
    if ((error && error != std::errc::no_such_file_or_directory) ||
        actual.type() != std::filesystem::file_type::not_found) {
        return Status(StatusCode::DataLoss, {});
    }
    return segment->capacity.settle_aborted_after_cleanup();
}

}  // namespace

Result<bool> PipelineController::rotation_due(
    const SegmentPolicy& policy, std::uint64_t now_monotonic) const {
    if ((policy.max_duration && policy.max_duration->count() <= 0) ||
        (policy.max_physical_bytes && *policy.max_physical_bytes == 0)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (!state_->prepared || !state_->output->instance->gate.snapshot().open) {
        return false;
    }
    std::shared_ptr<RecordingSegment> current;
    {
        std::lock_guard<std::mutex> slots(state_->segments);
        if (state_->successor) {
            return true;
        }
        if (state_->retained || state_->artifact_work.load() != 0) {
            return false;
        }
        current = std::atomic_load(&state_->output->segment);
    }
    if (!current) {
        return false;
    }
    const auto started = state_->segment_started_at.load();
    if (now_monotonic < started) {
        // This observation preceded a concurrent successful cut. It cannot
        // age the new Segment and must not poison newer clock authority.
        return false;
    }
    if (policy.max_duration &&
        now_monotonic - started >= static_cast<std::uint64_t>(policy.max_duration->count())) {
        return true;
    }
    const auto facts = current->capacity.snapshot();
    const auto& envelope = state_->options.envelope;
    if (facts.records == 0) {
        return false;
    }
    if (policy.max_physical_bytes && facts.actual_bytes >= *policy.max_physical_bytes) {
        return true;
    }
    if (facts.records >= envelope.max_segment_records ||
        facts.input_bytes >= envelope.max_segment_input_bytes ||
        envelope.max_record_bytes > envelope.max_segment_input_bytes - facts.input_bytes) {
        return true;
    }
    // Conservative one-more-record boundary, using the actual frozen model.
    // Never use average compression or filesystem usage to widen Admission.
    const auto& model = state_->output->writer->capacity_model();
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    if (model.payload_expansion_numerator != 0 &&
        envelope.max_record_bytes > kMax / model.payload_expansion_numerator) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto product = envelope.max_record_bytes * model.payload_expansion_numerator;
    if (model.payload_expansion_denominator == 0) {
        return Status(StatusCode::DataLoss, {});
    }
    auto bound = product / model.payload_expansion_denominator +
                 (product % model.payload_expansion_denominator != 0 ? 1 : 0);
    if (!add(bound, model.per_record_overhead_bytes) || !add(bound, model.seal_reserve_bytes)) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto limit = envelope.max_segment_physical_bytes;
    return facts.storage_bound >= limit || bound > limit - facts.storage_bound;
}

Result<void> add_segment_records(
    SegmentMetadataSnapshot& aggregate, const SegmentMetadataSnapshot& segment,
    bool completed_only) {
    auto result = aggregate;
    if (!add(result.accepted, segment.accepted) || !add(result.written, segment.written) ||
        !add(result.logical_bytes, segment.logical_bytes) ||
        !add(result.known_failed, segment.known_failed) || !add(result.unknown, segment.unknown)) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (!completed_only) {
        if (!add(result.native_inflight, segment.native_inflight) ||
            !add(result.unresolved_owners, segment.unresolved_owners) ||
            !add(result.manifest_bytes_bound, segment.manifest_bytes_bound) ||
            !add(result.high_water_manifest_bytes_bound, segment.high_water_manifest_bytes_bound) ||
            !add(result.pending_admissions, segment.pending_admissions) ||
            !add(result.pending_descriptors, segment.pending_descriptors) ||
            !add(result.descriptors, segment.descriptors) || !add(result.epochs, segment.epochs)) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        result.closed = result.closed && segment.closed;
    }
    aggregate = result;
    return {};
}

Result<std::optional<SealedPipelineSegment>> PipelineController::rotate(
    std::uint64_t successor_segment_id, plugin::Deadline deadline) {
    PipelineControlCall call(state_->control);
    if (!call) {
        return Status(StatusCode::Conflict, {});
    }
    if (!state_->prepared || successor_segment_id == 0) {
        return Status(StatusCode::InvalidState, {});
    }
    {
        std::lock_guard<std::mutex> lock(state_->segments);
        if (state_->retained) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        if (state_->successor) {
            if (state_->successor->metadata.segment_id() != successor_segment_id) {
                return Status(StatusCode::Conflict, {});
            }
            // Native completion may be resumed after a superseding fence. It
            // settles only the existing cut and never re-opens Admission.
        }
    }
    if (state_->successor) {
        return state_->finish_rotation(deadline);
    }
    // The Gate is always acquired before Segment/Stream structure. Do not
    // inspect it under the Segment registry lock (the cut takes the reverse).
    if (!state_->output->instance->gate.snapshot().open) {
        return Status(StatusCode::InvalidState, {});
    }
    auto output = state_->output;
    auto old = std::atomic_load(&output->segment);
    if (!old || successor_segment_id == old->metadata.segment_id()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (state_->options.manifest.segment_sequence == std::numeric_limits<std::uint64_t>::max()) {
        (void)output->instance->establish_control_fence();
        return Status(StatusCode::ResourceExhausted, {});
    }
    try {
        std::vector<StreamAdmission*> streams;
        std::vector<DescriptorRef> descriptors;
        std::vector<StreamId> ids;
        const auto& bindings = output->instance->streams.bindings();
        streams.reserve(bindings.size());
        descriptors.reserve(bindings.size());
        ids.reserve(bindings.size());
        for (const auto& binding : bindings) {
            auto* stream = output->instance->streams.find(binding.stream_id);
            std::lock_guard<std::mutex> lock(stream->mutex);
            if (stream->descriptor_proposal_inflight || stream->segment != old) {
                return Status(StatusCode::Conflict, {});
            }
            streams.push_back(stream);
            descriptors.push_back(stream->active_descriptor);
            ids.push_back(binding.stream_id);
        }
        auto registration = state_->options.ledger->reserve_registration(successor_segment_id);
        if (!registration.ok()) {
            return registration.status();
        }
        auto sample = state_->options.receive_clock();
        if (!sample.ok()) {
            return sample.status();
        }
        auto identity = state_->options.manifest;
        identity.acquisition = state_->acquisition_library->binary();
        identity.storage = state_->storage_library->binary();
        identity.playback_anchor = state_->options.clock->anchor();
        identity.volume_id = state_->options.volume->identity();
        identity.segment_id = successor_segment_id;
        ++identity.segment_sequence;
        auto next = RecordingSegment::prepare(
            std::move(identity), std::move(ids), state_->options.envelope,
            output->writer->capacity_model(), state_->options.manifest_limits,
            *state_->options.capacity, std::move(registration.value()),
            sample.value().monotonic_ns);
        if (!next.ok()) {
            return next.status();
        }
        auto successor = std::move(next.value());
        auto prepared = output->execute(output, [output, successor, descriptors, sample, deadline] {
            for (const auto& descriptor : descriptors) {
                if (descriptor) {
                    auto ready = output->writer->prepare_descriptor(
                        successor, descriptor, sample.value().monotonic_ns, deadline);
                    if (!ready.ok()) {
                        return ready;
                    }
                }
            }
            return Result<void>{};
        });
        if (!prepared.ok()) {
            (void)abort_unopened(successor, state_->options.volume);
            return prepared.status();
        }
        auto cut_sample = state_->options.receive_clock();
        auto time_ready = cut_sample.ok()
                              ? state_->options.clock->capture_receive(cut_sample.value())
                              : Result<time::ClockSnapshot>(cut_sample.status());
        if (!time_ready.ok()) {
            auto aborted = abort_unopened(successor, state_->options.volume);
            output->fail(time_ready.status().code());
            return aborted.ok() ? time_ready.status() : aborted.status();
        }
        // Reserve the finite lock backing before the Gate. Gate -> all Stream
        // locks freezes Descriptor identity and swaps every writable cursor in
        // one short cut. There is no callback, allocation, Native or I/O here.
        std::vector<std::unique_lock<std::mutex>> locked;
        locked.reserve(streams.size());
        auto cut = output->instance->gate.enter_segment_cut(
            state_->options.identity.instance_id, state_->options.identity.control_generation);
        if (!cut.ok()) {
            auto aborted = abort_unopened(successor, state_->options.volume);
            return aborted.ok() ? cut.status() : aborted.status();
        }
        for (auto* stream : streams) {
            locked.emplace_back(stream->mutex);
        }
        bool unchanged = true;
        for (std::size_t index = 0; index < streams.size(); ++index) {
            unchanged = unchanged && !streams[index]->descriptor_proposal_inflight &&
                        streams[index]->active_descriptor == descriptors[index] &&
                        streams[index]->segment == old;
        }
        if (!unchanged) {
            locked.clear();
            cut.value().unlock();
            auto aborted = abort_unopened(successor, state_->options.volume);
            return aborted.ok() ? Status(StatusCode::Conflict, {}) : aborted.status();
        }
        {
            std::lock_guard<std::mutex> serial(output->mutex);
            output->rotating = true;
        }
        {
            std::lock_guard<std::mutex> slots(state_->segments);
            state_->successor = successor;
            state_->successor_started_at = cut_sample.value().monotonic_ns;
            old->close_admission();
            for (auto* stream : streams) {
                stream->segment_transition = true;
                stream->segment = successor;
            }
        }
        locked.clear();
        cut.value().unlock();
        return state_->finish_rotation(deadline);
    } catch (const std::bad_alloc&) {
        (void)output->instance->establish_control_fence();
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::optional<SealedPipelineSegment>> PipelineController::State::finish_rotation(
    plugin::Deadline deadline) {
    auto old = std::atomic_load(&output->segment);
    auto next = successor;
    if (!old || !next) {
        return Status(StatusCode::InvalidState, {});
    }
    if (rotation_seal_attempted) {
        return Status(StatusCode::Conflict, {});
    }
    auto idle = output->wait_idle(deadline);
    if (!idle.ok()) {
        return idle.status();
    }
    // Old candidates captured before the cut are rejected and unwind their
    // own backing. An observation bound is not permission to seal over them.
    if (old->capacity.snapshot().candidates != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    auto drained = output->execute(output, [owner = output, old, deadline] {
        for (;;) {
            auto pending = unsettled_record_count(old->metadata.snapshot());
            if (!pending.ok()) {
                return Result<void>(pending.status());
            }
            if (pending.value() == 0) {
                break;
            }
            auto step = owner->writer->write_available(owner->instance->queue, old, deadline);
            if (!step.ok()) {
                return Result<void>(step.status());
            }
            auto remaining = unsettled_record_count(old->metadata.snapshot());
            if (!remaining.ok()) {
                return Result<void>(remaining.status());
            }
            if (remaining.value() >= pending.value()) {
                return Result<void>(
                    Status(StatusCode::DataLoss, "Old Segment drain made no progress"));
            }
            if (step.value().next_segment &&
                *step.value().next_segment != old->metadata.segment_id()) {
                if (remaining.value() != 0) {
                    return Result<void>(
                        Status(StatusCode::DataLoss, "Old Segment accepted set is incomplete"));
                }
                break;
            }
        }
        return Result<void>{};
    });
    if (!drained.ok()) {
        return drained.status();
    }
    const auto facts = old->metadata.snapshot();
    if (facts.pending_admissions || facts.pending_descriptors || facts.native_inflight ||
        facts.unresolved_owners) {
        return Status(StatusCode::Unavailable, {});
    }
    std::shared_ptr<RotatedSegment> retired;
    if (facts.accepted != 0) {
        retired = std::make_shared<RotatedSegment>();
        if (artifact_work.load() == 0) {
            auto work = next_work_id();
            if (!work.ok()) {
                return work.status();
            }
            artifact_work = work.value();
        }
        const auto work = artifact_work.load();
        if (facts.unknown != 0 || facts.written == 0) {
            return Status(StatusCode::DataLoss, {});
        }
        retired->segment = old;
        retired->work_id = work;
        std::optional<storage::SegmentSealResult> seal;
        rotation_seal_attempted = true;
        auto sealed = output->execute(output, [owner = output, old, deadline, &seal] {
            auto result = owner->writer->seal(old, deadline);
            if (!result.ok()) {
                return Result<void>(result.status());
            }
            seal.emplace(std::move(result.value()));
            return Result<void>{};
        });
        if (!sealed.ok()) {
            return sealed.status();
        }
        retired->handoff.emplace(SealedPipelineSegment{
            old, std::move(*seal), options.envelope, options.manifest_limits, work});
    } else {
        auto aborted = abort_unopened(old, options.volume);
        if (!aborted.ok()) {
            return aborted.status();
        }
    }
    {
        std::lock_guard<std::mutex> slots(segments);
        retained = retired;
        std::atomic_store(&output->segment, next);
        successor.reset();
        options.manifest.segment_id = next->metadata.segment_id();
        ++options.manifest.segment_sequence;
        segment_started_at = successor_started_at;
        artifact_work = 0;
        artifact_settled = false;
        artifact_handed_off = false;
        empty_settled = false;
        rotation_seal_attempted = false;
    }
    for (const auto& binding : output->instance->streams.bindings()) {
        auto* stream = output->instance->streams.find(binding.stream_id);
        std::lock_guard<std::mutex> lock(stream->mutex);
        stream->segment_transition = false;
        stream->segment_changed.notify_all();
    }
    {
        std::lock_guard<std::mutex> serial(output->mutex);
        output->rotating = false;
    }
    output->wake();
    return retired ? retired->handoff : std::optional<SealedPipelineSegment>{};
}

}  // namespace rodaq::pipeline
