#pragma once

#include <cstdint>
#include <memory>

#include <rodaq/result.hpp>

namespace rodaq::artifact {

struct VolumeCapacityOptions {
    std::uint64_t max_managed_bytes{64ull * 1024 * 1024 * 1024};
    std::uint64_t max_commit_bytes{64ull * 1024 * 1024};
    std::uint64_t stop_free_bytes{64ull * 1024 * 1024};
    std::uint64_t space_sample_max_age_ns{2000000000};
    std::uint32_t max_segments{64};
    std::uint32_t max_commit_slots{16};
    std::uint32_t max_artifact_slots{4096};
};

struct VolumeCapacitySnapshot {
    std::uint64_t actual_bytes{0};
    std::uint64_t future_bytes{0};
    std::uint64_t commit_bytes{0};
    std::uint64_t high_water_commit_bytes{0};
    std::uint64_t consumed_since_sample{0};
    std::uint64_t sampled_free_bytes{0};
    std::uint64_t sampled_monotonic_ns{0};
    std::uint32_t segment_slots{0};
    std::uint32_t commit_slots{0};
    std::uint32_t artifact_slots{0};
    std::uint32_t orphaned_reservations{0};
    bool has_space_sample{false};
    bool actual_usage_complete{true};
    StatusCode protection{StatusCode::Ok};
};

struct VolumeCapacityState;
class VolumeCapacity;
class VolumeReservation;

// Only a sample begun by this Volume owner can update its space projection.
// Actual statvfs/mount checks run outside capacity and Pipeline locks.
class SpaceProbe {
private:
    friend class VolumeCapacity;
    SpaceProbe(
        std::weak_ptr<VolumeCapacityState> state, std::uint64_t id, std::uint64_t consumed,
        std::uint64_t started) noexcept;
    std::weak_ptr<VolumeCapacityState> state_;
    std::uint64_t id_;
    std::uint64_t consumed_;
    std::uint64_t started_;
};

// Actual managed data and its metadata slot survive commit/normal Catalog
// removal. Destruction does not claim deletion or release disk/ledger capacity.
class ArtifactCharge {
public:
    ArtifactCharge(const ArtifactCharge&) = delete;
    ArtifactCharge& operator=(const ArtifactCharge&) = delete;
    ArtifactCharge(ArtifactCharge&& other) noexcept;
    ArtifactCharge& operator=(ArtifactCharge&&) = delete;
    ~ArtifactCharge() = default;

    // Caller must already prove filesystem deletion and parent durability.
    Result<void> release_after_delete();
    // Caller must also prove tombstone audit/reference/durability GC conditions.
    Result<void> release_after_gc();

private:
    friend class VolumeReservation;
    friend class VolumeCapacity;
    ArtifactCharge(std::shared_ptr<VolumeCapacityState> state, std::uint64_t bytes) noexcept;
    std::shared_ptr<VolumeCapacityState> state_;
    std::uint64_t bytes_{0};
    bool deleted_{false};
};

// One Segment owner serializes this move-only lease. Shared Volume aggregates
// are internally synchronized; no operation performs I/O or invokes callbacks.
class VolumeReservation {
public:
    VolumeReservation(const VolumeReservation&) = delete;
    VolumeReservation& operator=(const VolumeReservation&) = delete;
    VolumeReservation(VolumeReservation&& other) noexcept;
    VolumeReservation& operator=(VolumeReservation&&) = delete;
    ~VolumeReservation() noexcept;

    Result<void> grow(std::uint64_t future_bytes, std::uint64_t now_monotonic_ns);
    // Rolls back unused candidate exposure; never releases actual usage.
    Result<void> release_future(std::uint64_t bytes);
    // Actual growth converts previously reserved exposure; total cannot grow.
    Result<void> record_usage(std::uint64_t actual_bytes, std::uint64_t remaining_future_bytes);
    void mark_effects_possible() noexcept;
    // Backend exceeded its envelope: aggregate actual usage is no longer a
    // complete disk fact. Retain all charges and stop further growth.
    void invalidate_usage();
    // Requires sealed/verified actual bytes, no in-flight native access and no
    // remaining future exposure. Caller owns commit/quarantine business proof.
    Result<ArtifactCharge> settle_retained();
    // Requires all files/committing facts durably cleaned; never observer expiry.
    Result<void> settle_aborted_after_cleanup();

private:
    friend class VolumeCapacity;
    VolumeReservation(
        std::shared_ptr<VolumeCapacityState> state, std::uint64_t future_bytes) noexcept;
    std::shared_ptr<VolumeCapacityState> state_;
    std::uint64_t actual_{0};
    std::uint64_t future_{0};
    bool effects_possible_{false};
};

// Runtime accounting projection only, not Volume identity, Ledger or Catalog
// authority. Composition bounds owners and supplies verified recovery totals.
class VolumeCapacity {
public:
    static Result<VolumeCapacity> create(
        VolumeCapacityOptions options, std::uint64_t initial_actual_bytes = 0,
        std::uint32_t initial_artifact_slots = 0);
    VolumeCapacity(const VolumeCapacity&) = delete;
    VolumeCapacity& operator=(const VolumeCapacity&) = delete;
    VolumeCapacity(VolumeCapacity&&) noexcept = default;
    VolumeCapacity& operator=(VolumeCapacity&&) noexcept = default;

    // Claims exactly one entry from the verified startup totals. The recovery
    // index supplies each Ledger/retained artifact once; this is not adoption
    // of Ledger-absent files or a second artifact-lifecycle authority.
    Result<ArtifactCharge> claim_recovered_artifact(std::uint64_t actual_bytes);
    Result<SpaceProbe> begin_space_sample(std::uint64_t started_monotonic_ns);
    Result<void> finish_space_sample(
        const SpaceProbe& probe, std::uint64_t free_bytes, std::uint64_t finished_monotonic_ns);
    // Invalidates outstanding probes too: a late pre-failure result cannot
    // restore admission. A new successful probe may restore space evidence.
    void invalidate_space_sample();
    Result<VolumeReservation> reserve(
        std::uint64_t initial_future_bytes, std::uint64_t now_monotonic_ns);
    void protect(StatusCode failure);
    VolumeCapacitySnapshot snapshot() const;

private:
    explicit VolumeCapacity(std::shared_ptr<VolumeCapacityState> state) noexcept;
    std::shared_ptr<VolumeCapacityState> state_;
};

}  // namespace rodaq::artifact
