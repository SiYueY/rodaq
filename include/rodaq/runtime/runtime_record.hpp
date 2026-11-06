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

    Result<void> set(std::size_t slot, std::int64_t reading);

    bool present(std::size_t slot) const noexcept;
    std::optional<std::int64_t> value(std::size_t slot) const noexcept;
    std::uint8_t present_mask() const noexcept { return present_mask_; }
    std::size_t slot_count() const noexcept { return kMaxSlots; }

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
    static Result<AcceptanceSequenceAllocator> create(
        std::size_t max_streams, std::uint64_t first_sequence = 0);

    AcceptanceSequenceAllocator(const AcceptanceSequenceAllocator&) = delete;
    AcceptanceSequenceAllocator& operator=(const AcceptanceSequenceAllocator&) = delete;
    AcceptanceSequenceAllocator(AcceptanceSequenceAllocator&& other) noexcept;
    AcceptanceSequenceAllocator& operator=(AcceptanceSequenceAllocator&&) = delete;

    bool can_allocate(StreamHandle stream) noexcept;
    Result<std::uint64_t> allocate(StreamHandle stream);

    std::size_t capacity() const noexcept { return entries_.size(); }
    std::size_t registered_stream_count() const;

private:
    struct Entry {
        StreamHandle stream;
        std::uint64_t next_sequence = 0;
        bool exhausted = false;
    };

    AcceptanceSequenceAllocator(std::size_t max_streams, std::uint64_t first_sequence);

    Entry* find_or_insert_locked(StreamHandle stream);
    Entry* find_locked(StreamHandle stream) noexcept;

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
    static Result<RuntimeRecord> create(
        BindingHandle binding, std::uint64_t acceptance_sequence,
        std::optional<SourceHandle> source, std::optional<std::uint64_t> source_sequence,
        TimestampState timestamps, Payload payload, Reservation reservation,
        std::int64_t accepted_steady_time_ns = 0);

    RuntimeRecord(const RuntimeRecord&) = delete;
    RuntimeRecord& operator=(const RuntimeRecord&) = delete;
    RuntimeRecord(RuntimeRecord&& other) noexcept;
    RuntimeRecord& operator=(RuntimeRecord&&) = delete;
    ~RuntimeRecord() = default;

    BindingHandle binding() const noexcept { return binding_; }
    std::uint64_t acceptance_sequence() const noexcept { return acceptance_sequence_; }
    std::int64_t accepted_steady_time_ns() const noexcept { return accepted_steady_time_ns_; }
    std::optional<SourceHandle> source() const noexcept { return source_; }
    std::optional<std::uint64_t> source_sequence() const noexcept { return source_sequence_; }
    const TimestampState& timestamps() const noexcept { return timestamps_; }
    const Payload& record_payload() const noexcept { return payload_; }
    const Reservation& resource_reservation() const noexcept { return reservation_; }
    Payload take_payload() noexcept;

private:
    RuntimeRecord(
        BindingHandle binding, std::uint64_t acceptance_sequence,
        std::optional<SourceHandle> source, std::optional<std::uint64_t> source_sequence,
        TimestampState timestamps, Payload payload, Reservation reservation,
        std::int64_t accepted_steady_time_ns) noexcept;

    BindingHandle binding_;
    std::uint64_t acceptance_sequence_ = 0;
    std::int64_t accepted_steady_time_ns_ = 0;
    std::optional<SourceHandle> source_;
    std::optional<std::uint64_t> source_sequence_;
    TimestampState timestamps_;
    Reservation reservation_;
    Payload payload_;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_RUNTIME_RECORD_HPP_
