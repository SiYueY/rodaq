#include "rodaq/runtime/runtime_record.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace rodaq {

namespace {

std::size_t hash_stream(StreamHandle stream) noexcept {
    std::uint64_t value = stream.Value();
    value ^= value >> 33U;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33U;
    return static_cast<std::size_t>(value);
}

}  // namespace

Result<void> TimestampState::Set(std::size_t slot, std::int64_t value) {
    if (slot >= kMaxSlots) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "timestamp slot is out of range"));
    }

    readings_[slot] = value;
    const auto bit = static_cast<std::uint8_t>(1U << slot);
    present_mask_ = static_cast<std::uint8_t>(present_mask_ | bit);
    return {};
}

bool TimestampState::Present(std::size_t slot) const noexcept {
    return slot < kMaxSlots && (present_mask_ & static_cast<std::uint8_t>(1U << slot)) != 0;
}

std::optional<std::int64_t> TimestampState::Value(std::size_t slot) const noexcept {
    if (!Present(slot)) {
        return std::nullopt;
    }
    return readings_[slot];
}

Result<AcceptanceSequenceAllocator> AcceptanceSequenceAllocator::Create(
    std::size_t max_streams, std::uint64_t first_sequence) {
    try {
        return AcceptanceSequenceAllocator(max_streams, first_sequence);
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "failed to allocate acceptance sequence state"));
    } catch (const std::length_error&) {
        return nonstd::make_unexpected(Error(
            ErrorCode::ResourceExhausted,
            "acceptance sequence stream count exceeds container limits"));
    }
}

AcceptanceSequenceAllocator::AcceptanceSequenceAllocator(
    std::size_t max_streams, std::uint64_t first_sequence)
: entries_(max_streams), first_sequence_(first_sequence) {}

AcceptanceSequenceAllocator::AcceptanceSequenceAllocator(
    AcceptanceSequenceAllocator&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    entries_ = std::move(other.entries_);
    entry_count_ = other.entry_count_;
    first_sequence_ = other.first_sequence_;
    other.entry_count_ = 0;
}

Result<std::uint64_t> AcceptanceSequenceAllocator::Allocate(StreamHandle stream) {
    if (!stream.Valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "stream handle is invalid"));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "acceptance sequence capacity is exhausted"));
    }

    Entry* entry = FindOrInsertLocked(stream);
    if (entry == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "acceptance sequence capacity is exhausted"));
    }
    if (entry->exhausted) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "acceptance sequence is exhausted for the stream"));
    }

    const std::uint64_t sequence = entry->next_sequence;
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
        entry->exhausted = true;
    } else {
        ++entry->next_sequence;
    }
    return sequence;
}

std::size_t AcceptanceSequenceAllocator::RegisteredStreamCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entry_count_;
}

AcceptanceSequenceAllocator::Entry* AcceptanceSequenceAllocator::FindOrInsertLocked(
    StreamHandle stream) {
    Entry* existing = FindLocked(stream);
    if (existing != nullptr) {
        return existing;
    }
    if (entry_count_ == entries_.size()) {
        return nullptr;
    }

    std::size_t index = hash_stream(stream) % entries_.size();
    while (entries_[index].stream.Valid()) {
        index = (index + 1U) % entries_.size();
    }
    entries_[index].stream = stream;
    entries_[index].next_sequence = first_sequence_;
    entries_[index].exhausted = false;
    ++entry_count_;
    return &entries_[index];
}

AcceptanceSequenceAllocator::Entry* AcceptanceSequenceAllocator::FindLocked(
    StreamHandle stream) noexcept {
    if (entries_.empty()) {
        return nullptr;
    }
    std::size_t index = hash_stream(stream) % entries_.size();
    for (std::size_t probe = 0; probe < entries_.size(); ++probe) {
        if (!entries_[index].stream.Valid()) {
            return nullptr;
        }
        if (entries_[index].stream == stream) {
            return &entries_[index];
        }
        index = (index + 1U) % entries_.size();
    }
    return nullptr;
}

Result<RuntimeRecord> RuntimeRecord::Create(
    BindingHandle binding, std::uint64_t acceptance_sequence, std::optional<SourceHandle> source,
    std::optional<std::uint64_t> source_sequence, TimestampState timestamps, Payload payload,
    Reservation reservation) {
    if (!binding.Valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "runtime record binding handle is invalid"));
    }
    if (source_sequence.has_value() && !source.has_value()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "source sequence requires a source handle"));
    }
    if (source.has_value() && !source->Valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "runtime record source handle is invalid"));
    }
    if (!reservation.Active()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "runtime record requires a resource reservation"));
    }

    return RuntimeRecord(
        binding, acceptance_sequence, source, source_sequence, timestamps, std::move(payload),
        std::move(reservation));
}

RuntimeRecord::RuntimeRecord(
    BindingHandle binding, std::uint64_t acceptance_sequence, std::optional<SourceHandle> source,
    std::optional<std::uint64_t> source_sequence, TimestampState timestamps, Payload payload,
    Reservation reservation) noexcept
: binding_(binding),
  acceptance_sequence_(acceptance_sequence),
  source_(source),
  source_sequence_(source_sequence),
  timestamps_(timestamps),
  reservation_(std::move(reservation)),
  payload_(std::move(payload)) {}

RuntimeRecord::RuntimeRecord(RuntimeRecord&& other) noexcept
: binding_(other.binding_),
  acceptance_sequence_(other.acceptance_sequence_),
  source_(other.source_),
  source_sequence_(other.source_sequence_),
  timestamps_(other.timestamps_),
  reservation_(std::move(other.reservation_)),
  payload_(std::move(other.payload_)) {}

}  // namespace rodaq
