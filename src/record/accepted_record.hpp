#ifndef RODAQ_RECORD_ACCEPTED_RECORD_HPP_
#define RODAQ_RECORD_ACCEPTED_RECORD_HPP_

#include <rodaq/result.hpp>
#include <admission/resource_budget.hpp>
#include <canonical/handle.hpp>
#include <canonical/timestamp_state.hpp>
#include <payload/payload.hpp>

#include <cstdint>
#include <optional>

namespace rodaq {

class AcceptedRecord final {
public:
    static Result<AcceptedRecord> create(
        BindingHandle binding, std::uint64_t acceptance_sequence,
        std::optional<SourceHandle> source, std::optional<std::uint64_t> source_sequence,
        TimestampState timestamps, Payload payload, Reservation reservation,
        std::int64_t accepted_steady_time_ns = 0);

    AcceptedRecord(const AcceptedRecord&) = delete;
    AcceptedRecord& operator=(const AcceptedRecord&) = delete;
    AcceptedRecord(AcceptedRecord&& other) noexcept;
    AcceptedRecord& operator=(AcceptedRecord&&) = delete;
    ~AcceptedRecord() = default;

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
    AcceptedRecord(
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

#endif  // RODAQ_RECORD_ACCEPTED_RECORD_HPP_
