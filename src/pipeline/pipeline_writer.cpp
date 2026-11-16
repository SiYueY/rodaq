#include "pipeline_writer.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <rodaq/storage/validation.hpp>

namespace rodaq::pipeline {

PipelineWriter::PipelineWriter(
    storage::Handle storage, storage::PrepareConfig envelope,
    std::shared_ptr<artifact::ManagedVolume> volume, AdmissionGate& gate,
    InstanceId instance) noexcept
: storage_(std::move(storage)),
  envelope_(envelope),
  volume_(std::move(volume)),
  gate_(gate),
  instance_(instance) {}

Result<std::unique_ptr<PipelineWriter>> PipelineWriter::prepare(
    storage::Handle storage, const storage::PrepareConfig& envelope,
    std::shared_ptr<artifact::ManagedVolume> volume, AdmissionGate& gate, InstanceId instance,
    std::uint32_t max_prepared_descriptors, plugin::Deadline deadline) {
    if (!volume || !valid_instance_id(instance) || !(gate.instance_id() == instance) ||
        gate.admission_fenced() || gate.snapshot().open ||
        !storage::abi::valid_prepare(&envelope) || max_prepared_descriptors == 0 ||
        max_prepared_descriptors > 65536 || envelope.max_batch_records > 65536 ||
        envelope.max_record_bytes > envelope.max_batch_bytes) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto verified = volume->verify();
    if (!verified.ok()) {
        return verified.status();
    }
    try {
        auto writer = std::unique_ptr<PipelineWriter>(
            new PipelineWriter(std::move(storage), envelope, std::move(volume), gate, instance));
        writer->catalog_capacity_ = max_prepared_descriptors;
        writer->catalog_.reserve(max_prepared_descriptors);
        writer->batch_.reserve(envelope.max_batch_records);
        writer->views_.resize(envelope.max_batch_records);
        auto prepared = writer->storage_.prepare(envelope, deadline);
        if (!prepared.ok()) {
            writer->fail(prepared.status().code());
            return prepared.status();
        }
        writer->model_ = prepared.value();
        return writer;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

void PipelineWriter::fail(StatusCode failure) noexcept {
    if (failure_ == StatusCode::Ok) {
        failure_ = failure == StatusCode::Ok ? StatusCode::Internal : failure;
    }
    (void)gate_.establish_control_fence(instance_);
}

Result<void> PipelineWriter::observe_usage(RecordingSegment& segment) {
    auto usage = storage_.segment_usage();
    if (!usage.ok()) {
        segment.capacity.invalidate_usage();
        fail(usage.status().code());
        return usage.status();
    }
    auto observed = segment.capacity.observe_usage(usage.value());
    if (!observed.ok()) {
        fail(observed.status().code());
    }
    return observed;
}

Result<void> PipelineWriter::prepare_descriptor(
    std::shared_ptr<RecordingSegment> segment, DescriptorRef descriptor, std::uint64_t now,
    plugin::Deadline deadline) {
    if (!segment || !descriptor || quiescent_ || failure_ != StatusCode::Ok ||
        gate_.admission_fenced() || !(segment->instance_id == instance_) ||
        !(segment->volume_id == volume_->identity())) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto volume_failure = volume_->protection();
    if (volume_failure != StatusCode::Ok) {
        fail(volume_failure);
        return Status(volume_failure, {});
    }
    const auto cached = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto& entry) {
        return entry->stream_id == descriptor->stream_id &&
               entry->descriptor_id == descriptor->descriptor_id;
    });
    if (cached != catalog_.end() && (*cached)->facts.fingerprint != descriptor->facts.fingerprint) {
        return Status(StatusCode::Conflict, {});
    }
    if (cached == catalog_.end()) {
        if (catalog_.size() == catalog_capacity_) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        if (active_ && active_ != segment) {
            return Status(StatusCode::InvalidState, {});
        }
    }
    const auto& facts = descriptor->facts;
    const std::array<std::uint64_t, 7> sizes{
        facts.channel_name.size(),
        facts.message_encoding.size(),
        facts.schema_name.size(),
        facts.schema_encoding.size(),
        facts.schema.size(),
        facts.ros2_type_description_hash.size(),
        facts.offered_qos.size() * sizeof(rodaq_ros2_qos_profile_t)};
    std::uint64_t bytes = 0;
    for (const auto size : sizes) {
        if (size > std::numeric_limits<std::uint64_t>::max() - bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        bytes += size;
    }
    auto captured = artifact::capture_manifest_descriptor(descriptor->view(), 0);
    if (!captured.ok()) {
        return captured.status();
    }
    auto metadata = segment->metadata.reserve_descriptor(std::move(captured.value()));
    if (!metadata.ok()) {
        return metadata.status();
    }
    auto physical = segment->capacity.reserve_descriptor(bytes, now);
    if (!physical.ok()) {
        return physical.status();
    }
    auto transferred = physical.value().transfer();
    if (!transferred.ok()) {
        return transferred.status();
    }
    metadata.value().transfer();
    if (cached != catalog_.end()) {
        // Backend catalog is reusable across Segments. A successor reserves its
        // own physical/Manifest slots without misattributing the active file's
        // usage or repeating a Native mutation against that old Segment.
        return metadata.value().mark_prepared();
    }
    segment->capacity.mark_effects_possible();
    auto prepared = storage_.prepare_descriptor(descriptor->view(), deadline);
    if (!prepared.ok()) {
        fail(prepared.status().code());
    }
    auto observed = observe_usage(*segment);
    if (!prepared.ok()) {
        return prepared.status();
    }
    if (!observed.ok()) {
        return observed.status();
    }
    catalog_.push_back(std::move(descriptor));
    return metadata.value().mark_prepared();
}

Result<void> PipelineWriter::open(
    std::shared_ptr<RecordingSegment> segment, plugin::Deadline deadline) {
    const auto identity = segment->metadata.segment_id();
    if (active_) {
        return active_ == segment ? Result<void>{}
                                  : Result<void>{Status(StatusCode::InvalidState, {})};
    }
    active_ = segment;
    segment->capacity.mark_effects_possible();
    auto staging = volume_->create_staging(identity);
    if (!staging.ok()) {
        return staging.status();
    }
    try {
        const auto path = staging.value().string();
        if (path.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        const storage::SegmentSpec spec{
            {sizeof(spec)}, identity, {path.data(), static_cast<std::uint32_t>(path.size())}};
        auto opened = storage_.open_segment(spec, deadline);
        if (!opened.ok()) {
            return opened.status();
        }
        return observe_usage(*segment);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> PipelineWriter::settle(WriterStep& progress, std::uint32_t prefix, bool unknown) {
    for (std::size_t index = 0; index < batch_.size(); ++index) {
        const auto kind = index < prefix ? RecordSettlementKind::Written
                          : unknown      ? RecordSettlementKind::Unknown
                                         : RecordSettlementKind::KnownFailed;
        auto settled = batch_[index].settlement.finish(kind, batch_[index].payload.size());
        if (!settled.ok()) {
            fail(StatusCode::DataLoss);
            return settled.status();
        }
        if (kind == RecordSettlementKind::Written) {
            ++progress.written;
        } else if (kind == RecordSettlementKind::Unknown) {
            ++progress.unknown;
        } else {
            ++progress.known_failed;
        }
    }
    batch_.clear();  // Actual Storage access ended before any payload/charge release.
    return {};
}

Result<WriterStep> PipelineWriter::write_available(
    RecordQueue& queue, std::shared_ptr<RecordingSegment> segment, plugin::Deadline deadline) {
    if (!segment || quiescent_ || !batch_.empty() || !(segment->instance_id == instance_) ||
        !(segment->volume_id == volume_->identity())) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto identity = segment->metadata.segment_id();
    auto popped = queue.pop_batch(
        identity, envelope_.max_batch_records,
        failure_ == StatusCode::Ok ? envelope_.max_batch_bytes
                                   : std::numeric_limits<std::uint64_t>::max(),
        batch_);
    if (!popped.ok()) {
        fail(popped.status().code());
        return popped.status();
    }
    WriterStep progress;
    progress.next_segment = popped.value();
    if (batch_.empty()) {
        progress.failure = failure_;
        return progress;
    }
    for (std::size_t index = 0; index < batch_.size(); ++index) {
        if (!(batch_[index].instance_id == instance_) || batch_[index].segment_id != identity ||
            !(batch_[index].task_id == segment->task_id) ||
            batch_[index].pipeline_id != segment->pipeline_id) {
            fail(StatusCode::DataLoss);
            return Status(StatusCode::DataLoss, {});
        }
        views_[index] = batch_[index].view();
    }
    if (volume_->protection() != StatusCode::Ok) {
        fail(volume_->protection());
    }
    if (failure_ == StatusCode::Ok) {
        auto opened = open(segment, deadline);
        if (!opened.ok()) {
            fail(opened.status().code());
        }
    }
    if (failure_ != StatusCode::Ok) {
        auto settled = settle(progress, 0, false);
        if (!settled.ok()) {
            return settled.status();
        }
        progress.failure = failure_;
        return progress;
    }
    for (auto& record : batch_) {
        auto entered = record.settlement.mark_native_entered();
        if (!entered.ok()) {
            fail(StatusCode::DataLoss);
            return entered.status();
        }
    }
    const auto outcome =
        storage_.write_batch({views_.data(), static_cast<std::uint32_t>(batch_.size())}, deadline);
    if (!outcome.status.ok()) {
        fail(outcome.status.code());
    }
    // Even a non-OK call may confirm its whole prefix. The SDK conservatively
    // converts invalid/throwing native output into an entire Unknown batch.
    const auto settled = settle(
        progress, outcome.batch_result.confirmed_prefix,
        outcome.batch_result.tail_state == RODAQ_STORAGE_TAIL_UNKNOWN);
    if (!settled.ok()) {
        return settled.status();
    }
    (void)observe_usage(*segment);
    progress.failure = failure_;
    return progress;
}

Result<storage::SegmentSealResult> PipelineWriter::seal(
    std::shared_ptr<RecordingSegment> segment, plugin::Deadline deadline) {
    if (!segment) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto metadata = segment->metadata.snapshot();
    const auto physical = segment->capacity.snapshot();
    if (!batch_.empty() || quiescent_ || seal_attempted_ || active_ != segment ||
        !(segment->instance_id == instance_) || !metadata.closed || physical.writable ||
        physical.candidates != 0 || metadata.pending_records != 0 ||
        metadata.pending_descriptors != 0 || metadata.native_inflight != 0 ||
        metadata.unresolved_owners != 0 || metadata.unknown != 0 ||
        metadata.accepted != metadata.written + metadata.known_failed || metadata.written == 0) {
        return Status(StatusCode::InvalidState, {});
    }
    seal_attempted_ = true;
    auto verified = volume_->verify();
    if (!verified.ok()) {
        fail(verified.status().code());
        return verified.status();
    }
    auto sealed = storage_.seal_segment(deadline);
    if (!sealed.ok()) {
        fail(sealed.status().code());
        return sealed.status();
    }
    if (sealed.value().total_physical_bytes > envelope_.max_segment_physical_bytes ||
        sealed.value().total_physical_bytes >
            physical.accepted_storage_bound + model_.seal_reserve_bytes) {
        segment->capacity.invalidate_usage();
        fail(StatusCode::DataLoss);
        return Status(StatusCode::DataLoss, {});
    }
    active_.reset();
    seal_attempted_ = false;
    return sealed;
}

Result<void> PipelineWriter::quiesce(plugin::Deadline deadline) {
    if (quiescent_) {
        return {};
    }
    if (!batch_.empty() || (failure_ == StatusCode::Ok && active_)) {
        return Status(StatusCode::InvalidState, {});
    }
    auto result = storage_.quiesce(deadline);
    if (result.ok()) {
        quiescent_ = true;
        active_.reset();
    } else {
        fail(result.status().code());
    }
    return result;
}

}  // namespace rodaq::pipeline
