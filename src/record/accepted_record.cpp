#include "record/accepted_record.hpp"

#include <utility>

namespace rodaq {

Result<AcceptedRecord> AcceptedRecord::create(
    BindingHandle binding, std::uint64_t acceptance_sequence, std::optional<SourceHandle> source,
    std::optional<std::uint64_t> source_sequence, TimestampState timestamps, Payload payload,
    Reservation reservation, std::int64_t accepted_steady_time_ns) {
    if (!binding.valid()) {
        return Status(StatusCode::InvalidArgument, "accepted record binding handle is invalid");
    }
    if (source_sequence.has_value() && !source.has_value()) {
        return Status(StatusCode::InvalidArgument, "source sequence requires a source handle");
    }
    if (source.has_value() && !source->valid()) {
        return Status(StatusCode::InvalidArgument, "accepted record source handle is invalid");
    }
    if (!reservation.active()) {
        return Status(StatusCode::InvalidArgument, "accepted record requires a reservation");
    }
    return AcceptedRecord(
        binding, acceptance_sequence, source, source_sequence, timestamps, std::move(payload),
        std::move(reservation), accepted_steady_time_ns);
}

AcceptedRecord::AcceptedRecord(
    BindingHandle binding, std::uint64_t acceptance_sequence, std::optional<SourceHandle> source,
    std::optional<std::uint64_t> source_sequence, TimestampState timestamps, Payload payload,
    Reservation reservation, std::int64_t accepted_steady_time_ns) noexcept
: binding_(binding),
  acceptance_sequence_(acceptance_sequence),
  accepted_steady_time_ns_(accepted_steady_time_ns),
  source_(source),
  source_sequence_(source_sequence),
  timestamps_(timestamps),
  reservation_(std::move(reservation)),
  payload_(std::move(payload)) {}

AcceptedRecord::AcceptedRecord(AcceptedRecord&& other) noexcept
: binding_(other.binding_),
  acceptance_sequence_(other.acceptance_sequence_),
  accepted_steady_time_ns_(other.accepted_steady_time_ns_),
  source_(other.source_),
  source_sequence_(other.source_sequence_),
  timestamps_(other.timestamps_),
  reservation_(std::move(other.reservation_)),
  payload_(std::move(other.payload_)) {}

Payload AcceptedRecord::take_payload() noexcept { return std::move(payload_); }

}  // namespace rodaq
