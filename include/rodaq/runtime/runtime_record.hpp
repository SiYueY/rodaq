#ifndef RODAQ_RUNTIME_RUNTIME_RECORD_HPP_
#define RODAQ_RUNTIME_RUNTIME_RECORD_HPP_

#include <rodaq/result.hpp>
#include <rodaq/runtime/handle.hpp>
#include <rodaq/runtime/payload.hpp>
#include <rodaq/runtime/resource_budget.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace rodaq {

/// Fixed-capacity timestamp state in StreamDescriptor slot order.
///
/// Binding state owns slot interpretation. A present value of zero remains
/// distinct from an absent value.
class TimestampState final {
public:
    static constexpr std::size_t kMaxSlots = 4;

    Result<void> Set(std::size_t slot, std::int64_t value);

    bool Present(std::size_t slot) const noexcept;
    std::optional<std::int64_t> Value(std::size_t slot) const noexcept;
    std::uint8_t PresentMask() const noexcept { return present_mask_; }
    std::size_t SlotCount() const noexcept { return kMaxSlots; }

private:
    std::uint8_t present_mask_ = 0;
    std::array<std::int64_t, kMaxSlots> readings_{};
};

/// Allocates monotonic AcceptanceSequence values independently per Stream.
///
/// Sequence zero is valid. After a Stream consumes UINT64_MAX, further
/// allocation for that Stream fails permanently until the allocator is
/// destroyed with its activation.
class AcceptanceSequenceAllocator final {
public:
    static Result<AcceptanceSequenceAllocator> Create(
        std::size_t max_streams, std::uint64_t first_sequence = 0);

    AcceptanceSequenceAllocator(const AcceptanceSequenceAllocator&) = delete;
    AcceptanceSequenceAllocator& operator=(const AcceptanceSequenceAllocator&) = delete;
    AcceptanceSequenceAllocator(AcceptanceSequenceAllocator&& other) noexcept;
    AcceptanceSequenceAllocator& operator=(AcceptanceSequenceAllocator&&) = delete;

    Result<std::uint64_t> Allocate(StreamHandle stream);

    std::size_t Capacity() const noexcept { return entries_.size(); }
    std::size_t RegisteredStreamCount() const;

private:
    struct Entry {
        StreamHandle stream;
        std::uint64_t next_sequence = 0;
        bool exhausted = false;
    };

    AcceptanceSequenceAllocator(std::size_t max_streams, std::uint64_t first_sequence);

    Entry* FindOrInsertLocked(StreamHandle stream);
    Entry* FindLocked(StreamHandle stream) noexcept;

    std::vector<Entry> entries_;
    std::size_t entry_count_ = 0;
    std::uint64_t first_sequence_ = 0;
    mutable std::mutex mutex_;
};

/// The immutable Runtime representation of one accepted Canonical Record.
///
/// A RuntimeRecord owns its Payload and ResourceBudget reservation. Destruction
/// releases the Payload before the reservation.
class RuntimeRecord final {
public:
    static Result<RuntimeRecord> Create(
        BindingHandle binding, std::uint64_t acceptance_sequence,
        std::optional<SourceHandle> source, std::optional<std::uint64_t> source_sequence,
        TimestampState timestamps, Payload payload, Reservation reservation);

    RuntimeRecord(const RuntimeRecord&) = delete;
    RuntimeRecord& operator=(const RuntimeRecord&) = delete;
    RuntimeRecord(RuntimeRecord&& other) noexcept;
    RuntimeRecord& operator=(RuntimeRecord&&) = delete;
    ~RuntimeRecord() = default;

    BindingHandle Binding() const noexcept { return binding_; }
    std::uint64_t AcceptanceSequence() const noexcept { return acceptance_sequence_; }
    std::optional<SourceHandle> Source() const noexcept { return source_; }
    std::optional<std::uint64_t> SourceSequence() const noexcept { return source_sequence_; }
    const TimestampState& Timestamps() const noexcept { return timestamps_; }
    const Payload& RecordPayload() const noexcept { return payload_; }
    const Reservation& ResourceReservation() const noexcept { return reservation_; }

private:
    RuntimeRecord(
        BindingHandle binding, std::uint64_t acceptance_sequence,
        std::optional<SourceHandle> source, std::optional<std::uint64_t> source_sequence,
        TimestampState timestamps, Payload payload, Reservation reservation) noexcept;

    BindingHandle binding_;
    std::uint64_t acceptance_sequence_ = 0;
    std::optional<SourceHandle> source_;
    std::optional<std::uint64_t> source_sequence_;
    TimestampState timestamps_;
    Reservation reservation_;
    Payload payload_;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_RUNTIME_RECORD_HPP_
