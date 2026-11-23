#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

#include "../artifact/manifest.hpp"

namespace rodaq::pipeline {

struct SegmentMetadataState;
class SegmentMetadata;
class RecordMetadataReservation;

enum class RecordSettlementKind { Written, KnownFailed, Unknown };

// Moves with the actual Record/call frame. Only actual outcomes classify it;
// observer expiration never finishes it. Loss of an unfinished owner records
// unresolved evidence and cannot produce a complete Manifest.
class RecordSettlement {
public:
    RecordSettlement() noexcept = default;
    RecordSettlement(const RecordSettlement&) = delete;
    RecordSettlement& operator=(const RecordSettlement&) = delete;
    RecordSettlement(RecordSettlement&& other) noexcept;
    RecordSettlement& operator=(RecordSettlement&& other) noexcept;
    ~RecordSettlement() noexcept;

    explicit operator bool() const noexcept;
    Result<void> mark_native_entered();
    Result<void> finish(RecordSettlementKind kind, std::uint64_t logical_bytes);

private:
    friend class RecordMetadataReservation;
    RecordSettlement(std::shared_ptr<SegmentMetadataState> state, std::size_t stream) noexcept;
    void abandon() noexcept;
    std::shared_ptr<SegmentMetadataState> state_;
    std::size_t stream_{0};
    bool native_entered_{false};
};

class RecordMetadataReservation {
public:
    RecordMetadataReservation(const RecordMetadataReservation&) = delete;
    RecordMetadataReservation& operator=(const RecordMetadataReservation&) = delete;
    RecordMetadataReservation(RecordMetadataReservation&& other) noexcept;
    RecordMetadataReservation& operator=(RecordMetadataReservation&&) = delete;
    ~RecordMetadataReservation() noexcept;

    // Internal precondition: valid increasing sequence from the locked Stream
    // allocator, after Gate/descriptor/Segment checks and all ordinary reserves.
    // Counts/ranges are fixed backing; no allocation, I/O or quota check follows.
    RecordSettlement accept(std::uint64_t sequence) noexcept;

private:
    friend class SegmentMetadata;
    RecordMetadataReservation(
        std::shared_ptr<SegmentMetadataState> state, std::size_t stream, std::size_t descriptor,
        std::size_t epoch, time::ClockSnapshot received, std::uint64_t recording_ns) noexcept;
    std::shared_ptr<SegmentMetadataState> state_;
    std::size_t stream_;
    std::size_t descriptor_;
    std::size_t epoch_;
    time::ClockSnapshot received_;
    std::uint64_t recording_ns_;
};

class DescriptorMetadataReservation {
public:
    DescriptorMetadataReservation(const DescriptorMetadataReservation&) = delete;
    DescriptorMetadataReservation& operator=(const DescriptorMetadataReservation&) = delete;
    DescriptorMetadataReservation(DescriptorMetadataReservation&& other) noexcept;
    DescriptorMetadataReservation& operator=(DescriptorMetadataReservation&&) = delete;
    ~DescriptorMetadataReservation() noexcept;

    // Transfer alongside physical ownership before actual Native descriptor
    // work. Actual success then makes the entry ready; failure keeps its bound.
    void transfer() noexcept;
    Result<void> mark_prepared();

private:
    friend class SegmentMetadata;
    DescriptorMetadataReservation(
        std::shared_ptr<SegmentMetadataState> state, std::size_t descriptor) noexcept;
    std::shared_ptr<SegmentMetadataState> state_;
    std::size_t descriptor_;
    bool transferred_{false};
};

struct SegmentMetadataSnapshot {
    std::uint64_t accepted{0};
    std::uint64_t written{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t known_failed{0};
    std::uint64_t unknown{0};
    // Records attached to write_batch frames; not a count/proof of all Native
    // descriptor/open/seal/quiesce operations on the Storage instance.
    std::uint64_t native_inflight{0};
    std::uint64_t unresolved_owners{0};
    std::uint64_t manifest_bytes_bound{0};
    std::uint64_t high_water_manifest_bytes_bound{0};
    // Pre-Admission reservations only. They leave this counter at Accept;
    // accepted-but-unsettled work is derived from the settlement counters.
    std::uint32_t pending_admissions{0};
    std::uint32_t pending_descriptors{0};
    std::uint32_t descriptors{0};
    std::uint32_t epochs{0};
    bool closed{false};
};

Result<std::uint64_t> unsettled_record_count(const SegmentMetadataSnapshot& snapshot);

// Per-Segment fixed metadata capacity, separate from business Gate authority.
// One short mutex owns reservations and accepted/settled facts. No method calls
// Native, samples clocks, performs I/O or creates an execution domain.
class SegmentMetadata {
public:
    static Result<SegmentMetadata> create(
        artifact::ManifestIdentity identity, std::vector<StreamId> streams,
        const storage::PrepareConfig& envelope, artifact::ManifestLimits limits);
    SegmentMetadata(const SegmentMetadata&) = delete;
    SegmentMetadata& operator=(const SegmentMetadata&) = delete;
    SegmentMetadata(SegmentMetadata&&) noexcept = default;
    SegmentMetadata& operator=(SegmentMetadata&&) noexcept = default;

    Result<DescriptorMetadataReservation> reserve_descriptor(artifact::ManifestDescriptor facts);
    // Prepare only, before any accepted/candidate Record. Recharges the frozen
    // identity input against the same existing Manifest budget before Open.
    Result<void> bind_effective_configuration(
        std::string identity, persistence::Json configuration);
    Result<RecordMetadataReservation> reserve_record(
        StreamId stream_id, rodaq_descriptor_id_t descriptor_id,
        const time::ClockSnapshot& received, std::uint64_t recording_ns);
    // Called inside Gate cut or after close_and_capture; existing pre-reserved
    // guards finish before the cut, all later candidates roll back normally.
    void close_admission();
    SegmentMetadataSnapshot snapshot() const;
    // Only after closed, candidate rollback and exact actual settlement. Unknown
    // remains in evidence; Artifact Committer separately owns normal eligibility.
    Result<artifact::SegmentManifest> capture_manifest(
        std::vector<artifact::FileEvidence> files) const;
    std::uint64_t segment_id() const noexcept;

private:
    explicit SegmentMetadata(std::shared_ptr<SegmentMetadataState> state) noexcept;
    std::shared_ptr<SegmentMetadataState> state_;
};

}  // namespace rodaq::pipeline
