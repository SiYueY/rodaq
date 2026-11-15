#include "segment_capacity.hpp"

#include <cassert>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <utility>

#include <rodaq/storage/capacity.hpp>

namespace rodaq::pipeline {

struct SegmentCapacityState {
    std::mutex mutex;
    storage::PrepareConfig limits;
    storage::CapacityModel model;
    std::uint64_t manifest_bound;
    artifact::VolumeReservation volume;
    std::optional<artifact::LedgerRegistration> registration;
    SegmentCapacitySnapshot facts;
    bool settled{false};

    SegmentCapacityState(
        storage::PrepareConfig envelope, storage::CapacityModel capacity, std::uint64_t manifest,
        artifact::VolumeReservation reservation, artifact::LedgerRegistration registration_slot)
    : limits(envelope),
      model(capacity),
      manifest_bound(manifest),
      volume(std::move(reservation)),
      registration(std::move(registration_slot)) {}

    Status fail_contract() {
        volume.invalidate_usage();
        facts.protection = StatusCode::DataLoss;
        facts.writable = false;
        return Status(StatusCode::DataLoss, {});
    }
};

SegmentCandidate::SegmentCandidate(
    std::shared_ptr<SegmentCapacityState> state, std::uint64_t input, std::uint64_t physical,
    bool descriptor) noexcept
: state_(std::move(state)), input_(input), physical_(physical), descriptor_(descriptor) {}

SegmentCandidate::SegmentCandidate(SegmentCandidate&& other) noexcept
: state_(std::move(other.state_)),
  input_(other.input_),
  physical_(other.physical_),
  descriptor_(other.descriptor_) {}

SegmentCandidate::~SegmentCandidate() noexcept {
    if (!state_) {
        return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& facts = state_->facts;
    --facts.candidates;
    facts.storage_bound -= physical_;
    if (descriptor_) {
        --facts.descriptors;
    } else {
        --facts.records;
        facts.input_bytes -= input_;
    }
    const auto released = state_->volume.release_future(physical_);
    assert(released.ok());
    (void)released;
}

Result<void> SegmentCandidate::transfer() {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->facts.writable || state->facts.protection != StatusCode::Ok) {
            return Status(StatusCode::Conflict, {});
        }
        state->facts.accepted_storage_bound += physical_;
        --state->facts.candidates;
    }
    state_.reset();
    return {};
}

SegmentCapacity::SegmentCapacity(std::shared_ptr<SegmentCapacityState> state) noexcept
: state_(std::move(state)) {}

Result<SegmentCapacity> SegmentCapacity::create(
    rodaq_segment_id_t segment_id, storage::PrepareConfig limits, storage::CapacityModel model,
    std::uint64_t core_manifest_bound, artifact::VolumeCapacity& volume,
    artifact::LedgerRegistration registration, std::uint64_t now_monotonic_ns) {
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    if (segment_id == 0 || registration.segment_id() != segment_id ||
        limits.max_record_bytes == 0 || limits.max_segment_records == 0 ||
        limits.max_segment_input_bytes == 0 || limits.max_descriptors_per_segment == 0 ||
        model.payload_expansion_denominator == 0 || model.descriptor_expansion_denominator == 0 ||
        model.fixed_segment_overhead_bytes > limits.max_segment_physical_bytes ||
        model.seal_reserve_bytes >
            limits.max_segment_physical_bytes - model.fixed_segment_overhead_bytes ||
        core_manifest_bound >
            kMax - model.fixed_segment_overhead_bytes - model.seal_reserve_bytes) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto reserved = volume.reserve(
        model.fixed_segment_overhead_bytes + model.seal_reserve_bytes + core_manifest_bound,
        now_monotonic_ns);
    if (!reserved.ok()) {
        return reserved.status();
    }
    try {
        auto state = std::make_shared<SegmentCapacityState>(
            limits, model, core_manifest_bound, std::move(reserved.value()),
            std::move(registration));
        state->facts.segment_id = segment_id;
        state->facts.storage_bound = model.fixed_segment_overhead_bytes;
        state->facts.accepted_storage_bound = model.fixed_segment_overhead_bytes;
        return SegmentCapacity(std::move(state));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<SegmentCandidate> SegmentCapacity::reserve_record(std::uint64_t bytes, std::uint64_t now) {
    return reserve(bytes, now, false);
}

Result<SegmentCandidate> SegmentCapacity::reserve_descriptor(
    std::uint64_t bytes, std::uint64_t now) {
    return reserve(bytes, now, true);
}

Result<SegmentCandidate> SegmentCapacity::reserve(
    std::uint64_t bytes, std::uint64_t now, bool descriptor) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& facts = state_->facts;
    const auto& limits = state_->limits;
    if (!facts.writable || facts.protection != StatusCode::Ok) {
        return Status(StatusCode::InvalidState, {});
    }
    if (facts.candidates >= 65536 ||
        (descriptor && facts.descriptors >= limits.max_descriptors_per_segment) ||
        (!descriptor &&
         (bytes > limits.max_record_bytes || facts.records >= limits.max_segment_records ||
          bytes > limits.max_segment_input_bytes - facts.input_bytes))) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto bound = descriptor ? storage::descriptor_bound(state_->model, bytes)
                            : storage::record_bound(state_->model, bytes);
    if (!bound.ok()) {
        return bound.status();
    }
    if (bound.value() > limits.max_segment_physical_bytes - state_->model.seal_reserve_bytes -
                            facts.storage_bound) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto grown = state_->volume.grow(bound.value(), now);
    if (!grown.ok()) {
        return grown.status();
    }
    ++facts.candidates;
    facts.storage_bound += bound.value();
    if (descriptor) {
        ++facts.descriptors;
    } else {
        ++facts.records;
        facts.input_bytes += bytes;
    }
    return SegmentCandidate(state_, bytes, bound.value(), descriptor);
}

void SegmentCapacity::close_admission() {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->facts.writable = false;
    }
}

void SegmentCapacity::mark_effects_possible() {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->volume.mark_effects_possible();
    }
}

Result<void> SegmentCapacity::observe_usage(storage::SegmentUsage usage) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& facts = state_->facts;
    if (facts.sealed || state_->settled) {
        return Status(StatusCode::InvalidState, {});
    }
    if (usage.physical_bytes > facts.accepted_storage_bound ||
        usage.pending_physical_upper_bound > facts.accepted_storage_bound - usage.physical_bytes ||
        usage.file_count > state_->limits.max_segment_file_count) {
        return state_->fail_contract();
    }
    const auto future = facts.storage_bound + state_->model.seal_reserve_bytes +
                        state_->manifest_bound - usage.physical_bytes;
    auto updated = state_->volume.record_usage(usage.physical_bytes, future);
    if (!updated.ok()) {
        return state_->fail_contract();
    }
    facts.actual_bytes = usage.physical_bytes;
    return {};
}

Result<void> SegmentCapacity::finish_seal(
    std::uint64_t storage_bytes, std::uint64_t core_manifest_bytes) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& facts = state_->facts;
    if (facts.writable || facts.candidates != 0 || facts.sealed || state_->settled) {
        return Status(StatusCode::InvalidState, {});
    }
    if (facts.protection != StatusCode::Ok || core_manifest_bytes > state_->manifest_bound ||
        storage_bytes > facts.accepted_storage_bound + state_->model.seal_reserve_bytes) {
        return state_->fail_contract();
    }
    auto updated = state_->volume.record_usage(storage_bytes + core_manifest_bytes, 0);
    if (!updated.ok()) {
        return state_->fail_contract();
    }
    facts.actual_bytes = storage_bytes + core_manifest_bytes;
    facts.sealed = true;
    return {};
}

Result<artifact::LedgerRegistration> SegmentCapacity::take_registration_for_commit() {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->facts.sealed || state_->settled || !state_->registration) {
        return Status(StatusCode::InvalidState, {});
    }
    auto registration = std::move(*state_->registration);
    state_->registration.reset();
    return registration;
}

Result<artifact::ArtifactCharge> SegmentCapacity::settle_retained() {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->facts.sealed || state_->settled || state_->registration) {
        return Status(StatusCode::InvalidState, {});
    }
    auto retained = state_->volume.settle_retained();
    if (retained.ok()) {
        state_->settled = true;
    }
    return retained;
}

Result<void> SegmentCapacity::settle_aborted_after_cleanup() {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->facts.writable || state_->facts.candidates != 0 || state_->settled) {
        return Status(StatusCode::InvalidState, {});
    }
    auto cleaned = state_->volume.settle_aborted_after_cleanup();
    if (cleaned.ok()) {
        state_->registration.reset();
        state_->settled = true;
    }
    return cleaned;
}

SegmentCapacitySnapshot SegmentCapacity::snapshot() const {
    if (!state_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->facts;
}

}  // namespace rodaq::pipeline
