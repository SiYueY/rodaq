#include "volume_capacity.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace rodaq::artifact {

struct VolumeCapacityState {
    std::mutex mutex;
    VolumeCapacityOptions options;
    VolumeCapacitySnapshot counters;
    std::uint64_t consumed_total{0};
    std::uint64_t sample_consumed_baseline{0};
    std::uint64_t next_probe{1};
    std::uint64_t last_published_probe{0};
    std::uint64_t unclaimed_initial_bytes{0};
    std::uint32_t unclaimed_initial_slots{0};

    Status growth_status(std::uint64_t bytes, std::uint64_t now) const {
        if (counters.protection != StatusCode::Ok) {
            return Status(counters.protection, {});
        }
        if (!counters.has_space_sample ||
            (now >= counters.sampled_monotonic_ns &&
             now - counters.sampled_monotonic_ns > options.space_sample_max_age_ns)) {
            return Status(StatusCode::Unavailable, {});
        }
        if (counters.actual_bytes > options.max_managed_bytes ||
            counters.future_bytes > options.max_managed_bytes - counters.actual_bytes ||
            bytes > options.max_managed_bytes - counters.actual_bytes - counters.future_bytes ||
            bytes > options.max_commit_bytes - counters.commit_bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        const auto debt = consumed_total - sample_consumed_baseline;
        if (counters.sampled_free_bytes < options.stop_free_bytes ||
            debt > counters.sampled_free_bytes - options.stop_free_bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        const auto available = counters.sampled_free_bytes - options.stop_free_bytes - debt;
        if (counters.future_bytes > available || bytes > available - counters.future_bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        return {};
    }
};

SpaceProbe::SpaceProbe(
    std::weak_ptr<VolumeCapacityState> state, std::uint64_t id, std::uint64_t consumed,
    std::uint64_t started) noexcept
: state_(std::move(state)), id_(id), consumed_(consumed), started_(started) {}

ArtifactCharge::ArtifactCharge(
    std::shared_ptr<VolumeCapacityState> state, std::uint64_t bytes) noexcept
: state_(std::move(state)), bytes_(bytes) {}

ArtifactCharge::ArtifactCharge(ArtifactCharge&& other) noexcept
: state_(std::move(other.state_)),
  bytes_(std::exchange(other.bytes_, 0)),
  deleted_(other.deleted_) {}

Result<void> ArtifactCharge::release_after_delete() {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    assert(state_->counters.actual_bytes >= bytes_);
    state_->counters.actual_bytes -= bytes_;
    bytes_ = 0;
    deleted_ = true;
    // A subsequent space sample proves when reclaimed blocks are available.
    return {};
}

Result<void> ArtifactCharge::release_after_gc() {
    if (!state_ || !deleted_ || bytes_ != 0) {
        return Status(StatusCode::InvalidState, {});
    }
    auto state = std::move(state_);
    std::lock_guard<std::mutex> lock(state->mutex);
    assert(state->counters.artifact_slots != 0);
    --state->counters.artifact_slots;
    return {};
}

VolumeReservation::VolumeReservation(
    std::shared_ptr<VolumeCapacityState> state, std::uint64_t future_bytes) noexcept
: state_(std::move(state)), future_(future_bytes) {}

VolumeReservation::VolumeReservation(VolumeReservation&& other) noexcept
: state_(std::move(other.state_)),
  actual_(std::exchange(other.actual_, 0)),
  future_(std::exchange(other.future_, 0)),
  effects_possible_(other.effects_possible_) {}

VolumeReservation::~VolumeReservation() noexcept {
    if (!state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (effects_possible_ || actual_ != 0) {
        // Losing a live owner cannot pretend that physical work disappeared.
        // This is fail-closed diagnostic debt, not a hidden cleanup job.
        state_->counters.protection = StatusCode::DataLoss;
        state_->counters.actual_usage_complete = false;
        ++state_->counters.orphaned_reservations;
        return;
    }
    state_->counters.future_bytes -= future_;
    state_->counters.commit_bytes -= future_;
    --state_->counters.segment_slots;
    --state_->counters.commit_slots;
    --state_->counters.artifact_slots;
}

Result<void> VolumeReservation::grow(std::uint64_t bytes, std::uint64_t now_monotonic_ns) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto status = state_->growth_status(bytes, now_monotonic_ns);
    if (!status.ok()) {
        return status;
    }
    future_ += bytes;
    state_->counters.future_bytes += bytes;
    state_->counters.commit_bytes += bytes;
    state_->counters.high_water_commit_bytes =
        std::max(state_->counters.high_water_commit_bytes, state_->counters.commit_bytes);
    return {};
}

Result<void> VolumeReservation::release_future(std::uint64_t bytes) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (bytes > future_) {
        return Status(StatusCode::InvalidArgument, {});
    }
    future_ -= bytes;
    state_->counters.future_bytes -= bytes;
    state_->counters.commit_bytes -= bytes;
    return {};
}

Result<void> VolumeReservation::record_usage(
    std::uint64_t actual_bytes, std::uint64_t remaining_future_bytes) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto previous_exposure = actual_ + future_;
    if (actual_bytes > previous_exposure ||
        remaining_future_bytes > previous_exposure - actual_bytes) {
        state_->counters.protection = StatusCode::DataLoss;
        state_->counters.actual_usage_complete = false;
        return Status(StatusCode::DataLoss, {});
    }
    const auto consumed = actual_bytes > actual_ ? actual_bytes - actual_ : 0;
    if (consumed > std::numeric_limits<std::uint64_t>::max() - state_->consumed_total) {
        state_->counters.protection = StatusCode::ResourceExhausted;
        state_->counters.actual_usage_complete = false;
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto& counters = state_->counters;
    counters.actual_bytes = counters.actual_bytes - actual_ + actual_bytes;
    counters.future_bytes = counters.future_bytes - future_ + remaining_future_bytes;
    counters.commit_bytes =
        counters.commit_bytes - previous_exposure + actual_bytes + remaining_future_bytes;
    state_->consumed_total += consumed;
    actual_ = actual_bytes;
    future_ = remaining_future_bytes;
    effects_possible_ = effects_possible_ || actual_ != 0;
    return {};
}

void VolumeReservation::mark_effects_possible() noexcept { effects_possible_ = true; }

void VolumeReservation::invalidate_usage() {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->counters.actual_usage_complete = false;
        state_->counters.protection = StatusCode::DataLoss;
        effects_possible_ = true;
    }
}

Result<ArtifactCharge> VolumeReservation::settle_retained() {
    if (!state_ || future_ != 0) {
        return Status(StatusCode::InvalidState, {});
    }
    auto state = std::move(state_);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->counters.commit_bytes -= actual_;
        --state->counters.segment_slots;
        --state->counters.commit_slots;
    }
    return ArtifactCharge(std::move(state), std::exchange(actual_, 0));
}

Result<void> VolumeReservation::settle_aborted_after_cleanup() {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    auto state = std::move(state_);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->counters.actual_bytes -= actual_;
        state->counters.future_bytes -= future_;
        state->counters.commit_bytes -= actual_ + future_;
        --state->counters.segment_slots;
        --state->counters.commit_slots;
        --state->counters.artifact_slots;
    }
    actual_ = 0;
    future_ = 0;
    return {};
}

VolumeCapacity::VolumeCapacity(std::shared_ptr<VolumeCapacityState> state) noexcept
: state_(std::move(state)) {}

Result<VolumeCapacity> VolumeCapacity::create(
    VolumeCapacityOptions options, std::uint64_t initial_actual_bytes,
    std::uint32_t initial_artifact_slots) {
    if (options.max_managed_bytes == 0 || options.max_commit_bytes == 0 ||
        options.space_sample_max_age_ns == 0 || options.max_segments == 0 ||
        options.max_segments > 65536 || options.max_commit_slots == 0 ||
        options.max_commit_slots > 65536 || options.max_artifact_slots == 0 ||
        options.max_artifact_slots > 65536 || initial_artifact_slots > 65536 ||
        (initial_actual_bytes != 0 && initial_artifact_slots == 0)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_shared<VolumeCapacityState>();
        state->options = options;
        state->counters.actual_bytes = initial_actual_bytes;
        state->counters.artifact_slots = initial_artifact_slots;
        state->unclaimed_initial_bytes = initial_actual_bytes;
        state->unclaimed_initial_slots = initial_artifact_slots;
        return VolumeCapacity(std::move(state));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ArtifactCharge> VolumeCapacity::claim_recovered_artifact(std::uint64_t actual_bytes) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->unclaimed_initial_slots == 0 || actual_bytes > state_->unclaimed_initial_bytes ||
        (state_->unclaimed_initial_slots == 1 && actual_bytes != state_->unclaimed_initial_bytes)) {
        state_->counters.protection = StatusCode::DataLoss;
        return Status(StatusCode::DataLoss, {});
    }
    --state_->unclaimed_initial_slots;
    state_->unclaimed_initial_bytes -= actual_bytes;
    return ArtifactCharge(state_, actual_bytes);
}

Result<SpaceProbe> VolumeCapacity::begin_space_sample(std::uint64_t started_monotonic_ns) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->next_probe == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    return SpaceProbe(state_, state_->next_probe++, state_->consumed_total, started_monotonic_ns);
}

Result<void> VolumeCapacity::finish_space_sample(
    const SpaceProbe& probe, std::uint64_t free_bytes, std::uint64_t finished_monotonic_ns) {
    if (!state_ || probe.state_.lock() != state_) {
        return Status(StatusCode::Conflict, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (probe.id_ <= state_->last_published_probe || probe.consumed_ > state_->consumed_total ||
        finished_monotonic_ns < probe.started_ ||
        (state_->counters.has_space_sample &&
         finished_monotonic_ns < state_->counters.sampled_monotonic_ns)) {
        return Status(StatusCode::Conflict, {});
    }
    if (finished_monotonic_ns - probe.started_ > state_->options.space_sample_max_age_ns) {
        return Status(StatusCode::Unavailable, {});
    }
    state_->last_published_probe = probe.id_;
    state_->sample_consumed_baseline = probe.consumed_;
    state_->counters.sampled_free_bytes = free_bytes;
    // The measurement may have occurred anywhere inside the syscall window.
    // Use its start as the conservative freshness boundary, not a late return.
    state_->counters.sampled_monotonic_ns = probe.started_;
    state_->counters.has_space_sample = true;
    return {};
}

void VolumeCapacity::invalidate_space_sample() {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->counters.has_space_sample = false;
        state_->last_published_probe = state_->next_probe - 1;
    }
}

Result<VolumeReservation> VolumeCapacity::reserve(
    std::uint64_t initial_future_bytes, std::uint64_t now_monotonic_ns) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto status = state_->growth_status(initial_future_bytes, now_monotonic_ns);
    if (!status.ok()) {
        return status;
    }
    auto& counters = state_->counters;
    if (counters.segment_slots >= state_->options.max_segments ||
        counters.commit_slots >= state_->options.max_commit_slots ||
        counters.artifact_slots >= state_->options.max_artifact_slots) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    ++counters.segment_slots;
    ++counters.commit_slots;
    ++counters.artifact_slots;
    counters.future_bytes += initial_future_bytes;
    counters.commit_bytes += initial_future_bytes;
    counters.high_water_commit_bytes =
        std::max(counters.high_water_commit_bytes, counters.commit_bytes);
    return VolumeReservation(state_, initial_future_bytes);
}

void VolumeCapacity::protect(StatusCode failure) {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->counters.protection = failure == StatusCode::Ok ? StatusCode::Unavailable : failure;
    }
}

VolumeCapacitySnapshot VolumeCapacity::snapshot() const {
    if (!state_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto counters = state_->counters;
    counters.consumed_since_sample = state_->consumed_total - state_->sample_consumed_baseline;
    return counters;
}

}  // namespace rodaq::artifact
