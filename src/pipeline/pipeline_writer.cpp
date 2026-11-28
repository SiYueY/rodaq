#include "pipeline_writer.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

#include <fmt/format.h>
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
        segment.invalidate_usage();
        fail(usage.status().code());
        return usage.status();
    }
    auto observed = segment.observe_usage(usage.value());
    if (!observed.ok()) {
        fail(observed.status().code());
    }
    return observed;
}

Result<void> PipelineWriter::prepare_descriptor(
    std::shared_ptr<RecordingSegment> segment, DescriptorRef descriptor,
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
    if (cached == catalog_.end()) {
        if (catalog_.size() == catalog_capacity_) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        if (active_ && active_ != segment) {
            return Status(StatusCode::InvalidState, {});
        }
    }
    auto captured = artifact::capture_manifest_descriptor(
        descriptor->view(), 0, 1024 * 1024,
        segment->metadata.limits().max_descriptor_metadata_bytes);
    if (!captured.ok()) {
        return captured.status();
    }
    auto metadata = segment->metadata.reserve_descriptor(std::move(captured.value()));
    if (!metadata.ok()) {
        return metadata.status();
    }
    if (cached != catalog_.end()) {
        // Backend catalog is reusable across rotated Segments.
        metadata.value().transfer();
        return metadata.value().mark_prepared();
    }
    auto prepared = storage_.prepare_descriptor(descriptor->view(), deadline);
    if (!prepared.ok()) {
        if (prepared.status().code() == StatusCode::DeadlineExceeded) {
            // Observer timeout: native Storage call may still complete late, so keep
            // the reservation pinned rather than freeing it prematurely.
            metadata.value().transfer();
        }
        fail(prepared.status().code());
        return prepared.status();
    }
    metadata.value().transfer();
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
    segment->mark_effects_possible();
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
    batch_.clear();
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
    const auto physical = segment->capacity_snapshot();
    if (!batch_.empty() || quiescent_ || seal_attempted_ || active_ != segment ||
        !(segment->instance_id == instance_) || !metadata.closed || physical.writable ||
        physical.candidates != 0 || metadata.pending_admissions != 0 ||
        metadata.pending_descriptors != 0 || metadata.native_inflight != 0 ||
        metadata.unresolved_owners != 0 || metadata.unknown != 0 ||
        metadata.accepted != metadata.written + metadata.known_failed || metadata.written == 0) {
        return Status(
            StatusCode::InvalidState,
            fmt::format(
                "Seal requires settled closed Segment: batch={} quiescent={} attempted={} "
                "active={} closed={} writable={} candidates={} records={} descriptors={} "
                "native={} unresolved={} unknown={} accepted={} written={} failed={}",
                batch_.size(), quiescent_, seal_attempted_, active_ == segment, metadata.closed,
                physical.writable, physical.candidates, metadata.pending_admissions,
                metadata.pending_descriptors, metadata.native_inflight, metadata.unresolved_owners,
                metadata.unknown, metadata.accepted, metadata.written, metadata.known_failed));
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
    if (sealed.value().total_physical_bytes > physical.reserved_bytes) {
        segment->invalidate_usage();
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
