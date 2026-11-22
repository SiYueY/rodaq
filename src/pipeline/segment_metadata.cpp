#include "segment_metadata.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace rodaq::pipeline {
namespace {

struct DescriptorEntry {
    std::optional<artifact::ManifestDescriptor> facts;
    std::uint64_t size_bound{0};
    bool ready{false};
};

struct EpochEntry {
    std::optional<artifact::ManifestEpoch> facts;
    std::uint32_t pending{0};
};

bool same_jump(
    const std::optional<time::ClockJump>& left,
    const std::optional<time::ClockJump>& right) noexcept {
    return left.has_value() == right.has_value() &&
           (!left || (left->previous_epoch_id == right->previous_epoch_id &&
                      left->delta_ns == right->delta_ns &&
                      left->detected_monotonic_ns == right->detected_monotonic_ns));
}

bool valid_clock(const time::ClockSnapshot& received, const time::PlaybackAnchor& anchor) noexcept {
    if (received.boot_id != anchor.boot_id || received.epoch_id == 0 ||
        received.sample.monotonic_ns < anchor.monotonic_ns) {
        return false;
    }
    if (received.origin == time::ClockEpochOrigin::RealtimeJump) {
        return received.jump && received.jump->previous_epoch_id != 0 &&
               received.jump->previous_epoch_id != received.epoch_id &&
               received.jump->delta_ns != 0 &&
               received.jump->detected_monotonic_ns <= received.sample.monotonic_ns;
    }
    return !received.jump && (received.origin == time::ClockEpochOrigin::Boot ||
                              received.origin == time::ClockEpochOrigin::AuthorityRestart);
}

}  // namespace

Result<std::uint64_t> unsettled_record_count(const SegmentMetadataSnapshot& snapshot) {
    if (snapshot.written > snapshot.accepted ||
        snapshot.known_failed > snapshot.accepted - snapshot.written ||
        snapshot.unknown > snapshot.accepted - snapshot.written - snapshot.known_failed) {
        return Status(StatusCode::DataLoss, {});
    }
    return snapshot.accepted - snapshot.written - snapshot.known_failed - snapshot.unknown;
}

struct SegmentMetadataState {
    artifact::ManifestIdentity identity;
    artifact::ManifestLimits limits;
    std::uint64_t max_records{0};
    std::uint32_t max_files{0};
    std::uint32_t max_path_bytes{0};
    std::vector<artifact::StreamCoverage> streams;
    std::vector<DescriptorEntry> descriptors;
    std::vector<EpochEntry> epochs;
    mutable std::mutex mutex;
    SegmentMetadataSnapshot facts;
    std::uint64_t first_receive{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t last_receive{0};
    std::int64_t min_realtime{std::numeric_limits<std::int64_t>::max()};
    std::int64_t max_realtime{std::numeric_limits<std::int64_t>::min()};
    std::uint64_t first_recording{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t last_recording{0};

    bool fits(std::uint64_t bytes) const noexcept {
        return bytes <= limits.max_bytes - facts.manifest_bytes_bound;
    }

    void charge(std::uint64_t bytes) noexcept {
        facts.manifest_bytes_bound += bytes;
        facts.high_water_manifest_bytes_bound =
            std::max(facts.high_water_manifest_bytes_bound, facts.manifest_bytes_bound);
    }

    void cancel_record(std::size_t epoch) noexcept {
        --facts.pending_admissions;
        auto& entry = epochs[epoch];
        --entry.pending;
        if (entry.pending == 0 && entry.facts->accepted_records == 0) {
            entry.facts.reset();
            --facts.epochs;
            facts.manifest_bytes_bound -= artifact::kManifestEpochSizeBound;
        }
    }
};

RecordSettlement::RecordSettlement(
    std::shared_ptr<SegmentMetadataState> state, std::size_t stream) noexcept
: state_(std::move(state)), stream_(stream) {}

RecordSettlement::RecordSettlement(RecordSettlement&& other) noexcept
: state_(std::move(other.state_)), stream_(other.stream_), native_entered_(other.native_entered_) {}

RecordSettlement& RecordSettlement::operator=(RecordSettlement&& other) noexcept {
    if (this != &other) {
        abandon();
        state_ = std::move(other.state_);
        stream_ = other.stream_;
        native_entered_ = other.native_entered_;
    }
    return *this;
}

RecordSettlement::~RecordSettlement() noexcept { abandon(); }

void RecordSettlement::abandon() noexcept {
    if (!state_) {
        return;
    }
    auto state = std::move(state_);
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->facts.unresolved_owners;
    // Do not invent KnownFailed/Unknown or actual Native return. An entered
    // owner's native_inflight remains visible until reconciliation/restart.
}

RecordSettlement::operator bool() const noexcept { return static_cast<bool>(state_); }

Result<void> RecordSettlement::mark_native_entered() {
    if (!state_ || native_entered_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->facts.native_inflight;
    native_entered_ = true;
    return {};
}

Result<void> RecordSettlement::finish(RecordSettlementKind kind, std::uint64_t logical_bytes) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    if (kind != RecordSettlementKind::Written && kind != RecordSettlementKind::KnownFailed &&
        kind != RecordSettlementKind::Unknown) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (!native_entered_ && kind != RecordSettlementKind::KnownFailed) {
        return Status(StatusCode::InvalidState, {});
    }
    auto state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (kind == RecordSettlementKind::Written &&
        logical_bytes > std::numeric_limits<std::uint64_t>::max() - state->facts.logical_bytes) {
        return Status(StatusCode::DataLoss, {});
    }
    state_.reset();
    auto& coverage = state->streams[stream_];
    if (native_entered_) {
        --state->facts.native_inflight;
    }
    switch (kind) {
        case RecordSettlementKind::Written:
            state->facts.logical_bytes += logical_bytes;
            ++coverage.written;
            ++state->facts.written;
            break;
        case RecordSettlementKind::KnownFailed:
            ++coverage.known_failed;
            ++state->facts.known_failed;
            break;
        case RecordSettlementKind::Unknown:
            ++coverage.unknown;
            ++state->facts.unknown;
            break;
    }
    return {};
}

RecordMetadataReservation::RecordMetadataReservation(
    std::shared_ptr<SegmentMetadataState> state, std::size_t stream, std::size_t descriptor,
    std::size_t epoch, time::ClockSnapshot received, std::uint64_t recording_ns) noexcept
: state_(std::move(state)),
  stream_(stream),
  descriptor_(descriptor),
  epoch_(epoch),
  received_(received),
  recording_ns_(recording_ns) {}

RecordMetadataReservation::RecordMetadataReservation(RecordMetadataReservation&& other) noexcept
: state_(std::move(other.state_)),
  stream_(other.stream_),
  descriptor_(other.descriptor_),
  epoch_(other.epoch_),
  received_(other.received_),
  recording_ns_(other.recording_ns_) {}

RecordMetadataReservation::~RecordMetadataReservation() noexcept {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->cancel_record(epoch_);
    }
}

RecordSettlement RecordMetadataReservation::accept(std::uint64_t sequence) noexcept {
    assert(state_ && sequence != 0);
    auto state = std::move(state_);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        assert(!state->facts.closed);
        auto& coverage = state->streams[stream_];
        assert(coverage.accepted == 0 || sequence > coverage.last_sequence);
        --state->facts.pending_admissions;
        ++state->facts.accepted;

        if (coverage.accepted == 0) {
            coverage.first_sequence = sequence;
        }
        ++coverage.accepted;
        coverage.last_sequence = sequence;
        ++state->descriptors[descriptor_].facts->accepted_records;

        auto& epoch = state->epochs[epoch_];
        --epoch.pending;
        ++epoch.facts->accepted_records;
        epoch.facts->first_receive_monotonic =
            std::min(epoch.facts->first_receive_monotonic, received_.sample.monotonic_ns);
        epoch.facts->last_receive_monotonic =
            std::max(epoch.facts->last_receive_monotonic, received_.sample.monotonic_ns);
        epoch.facts->max_sample_error_bound_ns =
            std::max(epoch.facts->max_sample_error_bound_ns, received_.sample.error_bound_ns);

        state->first_receive = std::min(state->first_receive, received_.sample.monotonic_ns);
        state->last_receive = std::max(state->last_receive, received_.sample.monotonic_ns);
        state->min_realtime = std::min(state->min_realtime, received_.sample.realtime_ns);
        state->max_realtime = std::max(state->max_realtime, received_.sample.realtime_ns);
        state->first_recording = std::min(state->first_recording, recording_ns_);
        state->last_recording = std::max(state->last_recording, recording_ns_);
    }
    return RecordSettlement(std::move(state), stream_);
}

DescriptorMetadataReservation::DescriptorMetadataReservation(
    std::shared_ptr<SegmentMetadataState> state, std::size_t descriptor) noexcept
: state_(std::move(state)), descriptor_(descriptor) {}

DescriptorMetadataReservation::DescriptorMetadataReservation(
    DescriptorMetadataReservation&& other) noexcept
: state_(std::move(other.state_)),
  descriptor_(other.descriptor_),
  transferred_(other.transferred_) {}

DescriptorMetadataReservation::~DescriptorMetadataReservation() noexcept {
    if (!state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    --state_->facts.pending_descriptors;
    if (!transferred_) {
        auto& entry = state_->descriptors[descriptor_];
        state_->facts.manifest_bytes_bound -= entry.size_bound;
        entry.facts.reset();
        entry.size_bound = 0;
        --state_->facts.descriptors;
    }
}

void DescriptorMetadataReservation::transfer() noexcept {
    assert(state_ && !transferred_);
    transferred_ = true;
}

Result<void> DescriptorMetadataReservation::mark_prepared() {
    if (!state_ || !transferred_) {
        return Status(StatusCode::InvalidState, {});
    }
    auto state = std::move(state_);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->descriptors[descriptor_].ready = true;
    --state->facts.pending_descriptors;
    return {};
}

SegmentMetadata::SegmentMetadata(std::shared_ptr<SegmentMetadataState> state) noexcept
: state_(std::move(state)) {}

Result<SegmentMetadata> SegmentMetadata::create(
    artifact::ManifestIdentity identity, std::vector<StreamId> streams,
    const storage::PrepareConfig& envelope, artifact::ManifestLimits limits) {
    if (streams.empty() || streams.size() > limits.max_streams ||
        envelope.max_segment_records == 0 || envelope.max_segment_file_count == 0 ||
        envelope.max_segment_file_count > limits.max_files ||
        envelope.max_relative_path_bytes == 0 || envelope.max_relative_path_bytes > 4096) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto base = artifact::manifest_base_size_bound(identity, limits);
    if (!base.ok()) {
        return base.status();
    }
    const auto files = static_cast<std::uint64_t>(envelope.max_segment_file_count) *
                       artifact::manifest_file_size_bound(envelope.max_relative_path_bytes);
    const auto coverage =
        static_cast<std::uint64_t>(streams.size()) * artifact::kManifestStreamSizeBound;
    if (base.value() > limits.max_bytes || files > limits.max_bytes - base.value() ||
        coverage > limits.max_bytes - base.value() - files) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    try {
        std::sort(streams.begin(), streams.end());
        if (streams.front() == 0 ||
            std::adjacent_find(streams.begin(), streams.end()) != streams.end()) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto state = std::make_shared<SegmentMetadataState>();
        state->identity = std::move(identity);
        state->limits = limits;
        state->max_records = envelope.max_segment_records;
        state->max_files = envelope.max_segment_file_count;
        state->max_path_bytes = envelope.max_relative_path_bytes;
        state->streams.reserve(streams.size());
        for (const auto id : streams) {
            artifact::StreamCoverage entry;
            entry.stream_id = id;
            state->streams.push_back(entry);
        }
        state->descriptors.resize(limits.max_descriptors);
        state->epochs.resize(limits.max_epochs);
        state->charge(base.value() + files + coverage);
        return SegmentMetadata(std::move(state));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<DescriptorMetadataReservation> SegmentMetadata::reserve_descriptor(
    artifact::ManifestDescriptor facts) {
    if (facts.accepted_records != 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto bound = artifact::manifest_descriptor_size_bound(facts, state_->limits);
    if (!bound.ok()) {
        return bound.status();
    }
    // Include a comma even for the first entry, keeping the bound conservative.
    const auto charged = static_cast<std::uint64_t>(bound.value()) + 1;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->facts.closed) {
        return Status(StatusCode::InvalidState, {});
    }
    if (std::none_of(state_->streams.begin(), state_->streams.end(), [&](const auto& stream) {
            return stream.stream_id == facts.stream_id;
        })) {
        return Status(StatusCode::InvalidArgument, {});
    }
    for (const auto& entry : state_->descriptors) {
        if (entry.facts && entry.facts->stream_id == facts.stream_id &&
            entry.facts->descriptor_id == facts.descriptor_id) {
            return Status(StatusCode::Conflict, {});
        }
    }
    if (!state_->fits(charged)) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    for (std::size_t index = 0; index < state_->descriptors.size(); ++index) {
        auto& entry = state_->descriptors[index];
        if (!entry.facts) {
            entry.facts.emplace(std::move(facts));
            entry.size_bound = charged;
            entry.ready = false;
            state_->charge(charged);
            ++state_->facts.descriptors;
            ++state_->facts.pending_descriptors;
            return DescriptorMetadataReservation(state_, index);
        }
    }
    return Status(StatusCode::ResourceExhausted, {});
}

Result<RecordMetadataReservation> SegmentMetadata::reserve_record(
    StreamId stream_id, rodaq_descriptor_id_t descriptor_id, const time::ClockSnapshot& received,
    std::uint64_t recording_ns) {
    if (!valid_clock(received, state_->identity.playback_anchor)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto projected = time::project_playback_time(
        state_->identity.playback_anchor, received.boot_id, received.sample.monotonic_ns);
    if (!projected.ok()) {
        return projected.status();
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->facts.closed) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto stream = std::find_if(
        state_->streams.begin(), state_->streams.end(),
        [&](const auto& entry) { return entry.stream_id == stream_id; });
    const auto descriptor = std::find_if(
        state_->descriptors.begin(), state_->descriptors.end(), [&](const auto& entry) {
            return entry.ready && entry.facts && entry.facts->stream_id == stream_id &&
                   entry.facts->descriptor_id == descriptor_id;
        });
    if (stream == state_->streams.end() || descriptor == state_->descriptors.end()) {
        return Status(StatusCode::InvalidState, {});
    }
    if (state_->facts.pending_admissions >= 65536 ||
        state_->facts.accepted >= state_->max_records ||
        state_->facts.pending_admissions >= state_->max_records - state_->facts.accepted) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto epoch = std::find_if(state_->epochs.begin(), state_->epochs.end(), [&](const auto& entry) {
        return entry.facts && entry.facts->epoch_id == received.epoch_id;
    });
    if (epoch == state_->epochs.end()) {
        if (!state_->fits(artifact::kManifestEpochSizeBound)) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        epoch = std::find_if(state_->epochs.begin(), state_->epochs.end(), [](const auto& entry) {
            return !entry.facts;
        });
        if (epoch == state_->epochs.end()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        epoch->facts.emplace(artifact::ManifestEpoch{
            received.epoch_id, received.origin, received.jump, 0,
            std::numeric_limits<std::uint64_t>::max(), 0, 0});
        state_->charge(artifact::kManifestEpochSizeBound);
        ++state_->facts.epochs;
    } else if (
        epoch->facts->origin != received.origin || !same_jump(epoch->facts->jump, received.jump)) {
        return Status(StatusCode::DataLoss, {});
    }
    ++epoch->pending;
    ++state_->facts.pending_admissions;
    return RecordMetadataReservation(
        state_, static_cast<std::size_t>(stream - state_->streams.begin()),
        static_cast<std::size_t>(descriptor - state_->descriptors.begin()),
        static_cast<std::size_t>(epoch - state_->epochs.begin()), received, recording_ns);
}

void SegmentMetadata::close_admission() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->facts.closed = true;
}

SegmentMetadataSnapshot SegmentMetadata::snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->facts;
}

Result<artifact::SegmentManifest> SegmentMetadata::capture_manifest(
    std::vector<artifact::FileEvidence> files) const {
    try {
        if (files.size() > state_->max_files ||
            std::any_of(files.begin(), files.end(), [&](const auto& file) {
                return file.relative_path.size() > state_->max_path_bytes;
            })) {
            return Status(StatusCode::DataLoss, {});
        }
        artifact::SegmentManifest manifest;
        std::uint64_t reserved_bound = 0;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            const auto& facts = state_->facts;
            if (!facts.closed || facts.accepted == 0 || facts.pending_admissions != 0 ||
                facts.pending_descriptors != 0 || facts.native_inflight != 0 ||
                facts.unresolved_owners != 0 || facts.written > facts.accepted ||
                facts.known_failed > facts.accepted - facts.written ||
                facts.unknown != facts.accepted - facts.written - facts.known_failed) {
                return Status(StatusCode::InvalidState, {});
            }

            reserved_bound = facts.manifest_bytes_bound;
            manifest.identity = state_->identity;
            manifest.logical_bytes = facts.logical_bytes;
            manifest.files = std::move(files);
            for (const auto& stream : state_->streams) {
                if (stream.accepted != 0) {
                    manifest.coverage.push_back(stream);
                }
            }

            for (const auto& descriptor : state_->descriptors) {
                if (descriptor.facts && descriptor.facts->accepted_records != 0) {
                    manifest.descriptors.push_back(*descriptor.facts);
                }
            }

            for (const auto& epoch : state_->epochs) {
                if (epoch.facts && epoch.facts->accepted_records != 0) {
                    manifest.epochs.push_back(*epoch.facts);
                }
            }

            manifest.first_receive_monotonic = state_->first_receive;
            manifest.last_receive_monotonic = state_->last_receive;
            manifest.min_receive_realtime = state_->min_realtime;
            manifest.max_receive_realtime = state_->max_realtime;
            manifest.recording_begin_ns = state_->first_recording;
            manifest.recording_end_ns = state_->last_recording;
        }
        auto encoded = artifact::encode_manifest(manifest, state_->limits);
        if (!encoded.ok()) {
            return encoded.status().code() == StatusCode::InvalidArgument
                       ? Status(StatusCode::DataLoss, {})
                       : encoded.status();
        }
        if (encoded.value().size() > reserved_bound) {
            return Status(StatusCode::DataLoss, {});
        }
        return manifest;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

std::uint64_t SegmentMetadata::segment_id() const noexcept { return state_->identity.segment_id; }

}  // namespace rodaq::pipeline
