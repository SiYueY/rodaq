#pragma once

#include <cstdint>
#include <memory>

#include <rodaq/storage/types.hpp>

#include "../artifact/volume_capacity.hpp"
#include "../artifact/commit_reservation.hpp"

namespace rodaq::pipeline {

struct SegmentCapacityState;
class SegmentCapacity;

// A candidate owns ordinary Segment/Volume/Commit capacity before Admission.
// Transfer for a Record occurs inside the short Core commit domain, before
// assigning sequence or publishing. Descriptor work transfers before entering
// Native Storage, so actual in-flight work retains its bound after a timeout.
class SegmentCandidate {
public:
    SegmentCandidate(const SegmentCandidate&) = delete;
    SegmentCandidate& operator=(const SegmentCandidate&) = delete;
    SegmentCandidate(SegmentCandidate&& other) noexcept;
    SegmentCandidate& operator=(SegmentCandidate&&) = delete;
    ~SegmentCandidate() noexcept;

    Result<void> transfer();
    std::uint64_t physical_bound() const noexcept { return physical_; }

private:
    friend class SegmentCapacity;
    SegmentCandidate(
        std::shared_ptr<SegmentCapacityState> state, std::uint64_t input, std::uint64_t physical,
        bool descriptor) noexcept;
    std::shared_ptr<SegmentCapacityState> state_;
    std::uint64_t input_;
    std::uint64_t physical_;
    bool descriptor_;
};

struct SegmentCapacitySnapshot {
    rodaq_segment_id_t segment_id{0};
    std::uint64_t records{0};
    std::uint64_t input_bytes{0};
    std::uint64_t storage_bound{0};
    std::uint64_t accepted_storage_bound{0};
    std::uint64_t actual_bytes{0};
    std::uint32_t descriptors{0};
    std::uint32_t candidates{0};
    bool writable{true};
    bool sealed{false};
    StatusCode protection{StatusCode::Ok};
};

// One retained owner per Segment; Records bind its immutable numeric ID.
// The conservative bound is retained until seal rather than estimated down
// from compression ratios. No method calls Native Storage or performs I/O.
class SegmentCapacity {
public:
    static Result<SegmentCapacity> create(
        rodaq_segment_id_t segment_id, storage::PrepareConfig limits, storage::CapacityModel model,
        std::uint64_t core_manifest_bound, artifact::VolumeCapacity& volume,
        artifact::LedgerRegistration registration, std::uint64_t now_monotonic_ns);
    SegmentCapacity(const SegmentCapacity&) = delete;
    SegmentCapacity& operator=(const SegmentCapacity&) = delete;
    SegmentCapacity(SegmentCapacity&&) noexcept = default;
    SegmentCapacity& operator=(SegmentCapacity&&) noexcept = default;

    Result<SegmentCandidate> reserve_record(std::uint64_t input_bytes, std::uint64_t now);
    Result<SegmentCandidate> reserve_descriptor(std::uint64_t canonical_bytes, std::uint64_t now);
    // Called under the Gate's short Segment cut barrier before swapping the
    // writable cursor. Existing candidates then fail transfer and roll back.
    void close_admission();
    void mark_effects_possible();
    Result<void> observe_usage(storage::SegmentUsage usage);
    // Actual usage cannot be established, or the backend violated its bound.
    // Retains every reservation and protects Volume admission until recovery.
    void invalidate_usage();
    // Requires actual Seal return and verified Storage bytes. Transfers all
    // Volume/Commit/Ledger ownership together, retaining the Manifest reserve.
    Result<artifact::CommitReservation> handoff_sealed(std::uint64_t storage_bytes);
    Result<void> settle_aborted_after_cleanup();
    SegmentCapacitySnapshot snapshot() const;

private:
    explicit SegmentCapacity(std::shared_ptr<SegmentCapacityState> state) noexcept;
    Result<SegmentCandidate> reserve(std::uint64_t bytes, std::uint64_t now, bool descriptor);
    std::shared_ptr<SegmentCapacityState> state_;
};

}  // namespace rodaq::pipeline
