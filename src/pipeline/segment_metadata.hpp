#pragma once

#include <cstddef>
#include <cstdint>
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

    RecordSettlement accept(std::uint64_t sequence) noexcept;

private:
    friend class SegmentMetadata;
    RecordMetadataReservation(
        std::shared_ptr<SegmentMetadataState> state, std::size_t stream, std::size_t descriptor,
        time::ClockSample received, std::uint64_t recording_ns) noexcept;
    std::shared_ptr<SegmentMetadataState> state_;
    std::size_t stream_;
    std::size_t descriptor_;
    time::ClockSample received_;
    std::uint64_t recording_ns_;
};

class DescriptorMetadataReservation {
public:
    DescriptorMetadataReservation(const DescriptorMetadataReservation&) = delete;
    DescriptorMetadataReservation& operator=(const DescriptorMetadataReservation&) = delete;
    DescriptorMetadataReservation(DescriptorMetadataReservation&& other) noexcept;
    DescriptorMetadataReservation& operator=(DescriptorMetadataReservation&&) = delete;
    ~DescriptorMetadataReservation() noexcept;

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
    std::uint64_t native_inflight{0};
    std::uint64_t unresolved_owners{0};
    std::uint32_t pending_admissions{0};
    std::uint32_t pending_descriptors{0};
    std::uint32_t descriptors{0};
    bool closed{false};
};

Result<std::uint64_t> unsettled_record_count(const SegmentMetadataSnapshot& snapshot);

// Per-Segment fixed metadata capacity, separate from business Gate authority.
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
    Result<RecordMetadataReservation> reserve_record(
        StreamId stream_id, rodaq_descriptor_id_t descriptor_id, const time::ClockSample& received,
        std::uint64_t recording_ns);
    void close_admission();
    SegmentMetadataSnapshot snapshot() const;
    Result<artifact::SegmentManifest> capture_manifest(
        std::vector<artifact::FileEvidence> files, WallTime committed_at) const;
    std::uint64_t segment_id() const noexcept;
    TaskId task_id() const noexcept;

private:
    explicit SegmentMetadata(std::shared_ptr<SegmentMetadataState> state) noexcept;
    std::shared_ptr<SegmentMetadataState> state_;
};

}  // namespace rodaq::pipeline
