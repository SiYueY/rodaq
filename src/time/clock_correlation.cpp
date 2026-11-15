#include "clock_correlation.hpp"

#include <new>
#include <utility>

namespace rodaq::time {

ClockCorrelation::ClockCorrelation(
    std::unique_ptr<PlaybackAnchorStore> store, const ClockSample& initial,
    ClockPolicy policy) noexcept
: store_(std::move(store)),
  policy_(policy),
  range_(store_->initial_range()),
  next_epoch_(range_.begin + 1) {
    snapshot_.boot_id = store_->anchor().boot_id;
    snapshot_.epoch_id = range_.begin;
    snapshot_.sample = initial;
    epoch_started_monotonic_ = initial.monotonic_ns;
    snapshot_.origin =
        range_.begin == 1 ? ClockEpochOrigin::Boot : ClockEpochOrigin::AuthorityRestart;
}

Result<std::unique_ptr<ClockCorrelation>> ClockCorrelation::create(
    const std::filesystem::path& directory, const BootIdentity& boot,
    const ClockSample& initial_sample, BootArtifactEvidence artifacts, ClockPolicy policy,
    persistence::FileFaultHook faults) {
    auto valid = compare_correlation(initial_sample, initial_sample, policy.allowed_error_ns);
    if (!valid.ok()) {
        return valid.status();
    }
    auto stored = PlaybackAnchorStore::open(
        directory, boot, initial_sample, artifacts, policy.epoch_reservation_count,
        std::move(faults));
    if (!stored.ok()) {
        return stored.status();
    }
    try {
        return std::unique_ptr<ClockCorrelation>(
            new ClockCorrelation(std::move(stored).value(), initial_sample, policy));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

PlaybackAnchor ClockCorrelation::anchor() const noexcept { return store_->anchor(); }

Result<ClockSnapshot> ClockCorrelation::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (failure_ != StatusCode::Ok) {
        return Status(failure_, {});
    }
    return snapshot_;
}

Result<ClockSnapshot> ClockCorrelation::capture_receive(const ClockSample& sample) const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    if (failure_ != StatusCode::Ok) {
        return Status(failure_, {});
    }
    if (sample.monotonic_ns < epoch_started_monotonic_) {
        return Status(StatusCode::Unavailable, {});
    }
    // A concurrent periodic observation may be slightly newer than the Core
    // receive sample. Compare chronologically without fabricating a new time.
    auto change = sample.monotonic_ns < snapshot_.sample.monotonic_ns
                      ? compare_correlation(sample, snapshot_.sample, policy_.allowed_error_ns)
                      : compare_correlation(snapshot_.sample, sample, policy_.allowed_error_ns);
    if (!change.ok()) {
        return std::move(change).status();
    }
    if (change.value().discontinuity) {
        // Only the observation lane may publish a new Epoch or refill its
        // durable range. Do not label a new correlation with the old Epoch.
        return Status(StatusCode::Unavailable, {});
    }
    auto captured = snapshot_;
    captured.sample = sample;
    return captured;
}

void ClockCorrelation::protect(StatusCode failure) {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    failure_ = failure;
}

Result<void> ClockCorrelation::observe(const ClockSample& sample) {
    std::lock_guard<std::mutex> execution(observation_);
    auto current = snapshot();
    if (!current.ok()) {
        return current.status();
    }
    auto change = compare_correlation(current.value().sample, sample, policy_.allowed_error_ns);
    if (!change.ok()) {
        // An obsolete observation does not invalidate the newer published fact.
        if (change.status().code() != StatusCode::InvalidArgument) {
            protect(change.status().code());
        }
        return change.status();
    }
    auto updated = current.value();
    updated.sample = sample;
    if (change.value().discontinuity) {
        if (next_epoch_ == range_.end_exclusive) {
            auto reserved = store_->reserve_epochs();
            if (!reserved.ok()) {
                protect(reserved.status().code());
                return reserved.status();
            }
            range_ = reserved.value();
            next_epoch_ = range_.begin;
        }
        updated.epoch_id = next_epoch_++;
        updated.origin = ClockEpochOrigin::RealtimeJump;
        updated.jump =
            ClockJump{current.value().epoch_id, change.value().delta_ns, sample.monotonic_ns};
    }
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        snapshot_ = updated;
        if (change.value().discontinuity) {
            epoch_started_monotonic_ = sample.monotonic_ns;
        }
    }
    return {};
}

}  // namespace rodaq::time
