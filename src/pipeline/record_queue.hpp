#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <rodaq/result.hpp>

#include "record.hpp"

namespace rodaq::pipeline {

struct RecordQueueState;
class RecordQueue;

class QueueReservation {
public:
    QueueReservation(const QueueReservation&) = delete;
    QueueReservation& operator=(const QueueReservation&) = delete;
    QueueReservation(QueueReservation&& other) noexcept;
    QueueReservation& operator=(QueueReservation&& other) noexcept;
    ~QueueReservation() noexcept;

private:
    friend class RecordQueue;
    explicit QueueReservation(std::shared_ptr<RecordQueueState> state) noexcept;
    void release() noexcept;
    std::shared_ptr<RecordQueueState> state_;
};

struct RecordQueueSnapshot {
    std::size_t capacity{0};
    std::size_t queued_records{0};
    std::size_t reserved_slots{0};
    std::size_t high_water_records{0};
    std::uint64_t queued_logical_bytes{0};
    std::uint64_t high_water_logical_bytes{0};
    std::optional<std::uint64_t> oldest_receive_monotonic;
    bool accepting{false};
};

// Capacity is reserved independently of ring position. Only actual Admission
// Commit assigns a publish position, preserving sequence order within a Stream.
// The instance owner retains this queue until accepted work is drained; it must
// not use destruction as a business Stop or discard an unconsumed accepted set.
class RecordQueue {
public:
    static Result<RecordQueue> create(std::size_t capacity);
    RecordQueue(const RecordQueue&) = delete;
    RecordQueue& operator=(const RecordQueue&) = delete;
    RecordQueue(RecordQueue&&) noexcept = default;
    RecordQueue& operator=(RecordQueue&&) noexcept = default;

    Result<QueueReservation> reserve();
    // Internal precondition: reservation belongs to this queue and Record is
    // fully owned/valid. No allocation, quota discovery, callback or I/O follows.
    void publish(QueueReservation reservation, Record record) noexcept;
    std::optional<Record> pop();
    // Moves only the contiguous front set for this already-reserved Segment.
    // Destination must be empty with max_records capacity prepared beforehand;
    // no allocation or backing release occurs while holding the queue lock.
    Result<std::optional<rodaq_segment_id_t>> pop_batch(
        rodaq_segment_id_t segment, std::size_t max_records, std::uint64_t max_bytes,
        std::vector<Record>& destination);
    void stop_reservations();
    RecordQueueSnapshot snapshot() const;

private:
    explicit RecordQueue(std::shared_ptr<RecordQueueState> state) noexcept;
    std::shared_ptr<RecordQueueState> state_;
};

}  // namespace rodaq::pipeline
