#ifndef RODAQ_CLOCK_CLOCK_CORRELATION_HPP_
#define RODAQ_CLOCK_CLOCK_CORRELATION_HPP_

#include <rodaq/plugin/abi/runtime.h>
#include <rodaq/result.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace rodaq {

struct UnixEpochClockAnchor {
    rodaq_clock_handle clock;
    std::int64_t clock_reading;
    std::uint64_t unix_anchor_ns;
};

using ClockReadingFunction = std::function<std::int64_t()>;
using UnixTimeFunction = std::function<std::uint64_t()>;

Result<std::uint64_t> project_unix_nanoseconds(
    std::int64_t reading, const UnixEpochClockAnchor& anchor);

class ClockCorrelation final {
public:
    static Result<ClockCorrelation> establish(
        rodaq_clock_handle clock, ClockReadingFunction clock_reading, UnixTimeFunction unix_time);

    static Result<ClockCorrelation> from_anchor(UnixEpochClockAnchor anchor);

    ClockCorrelation(const ClockCorrelation&) = delete;
    ClockCorrelation& operator=(const ClockCorrelation&) = delete;
    ClockCorrelation(ClockCorrelation&&) noexcept;
    ClockCorrelation& operator=(ClockCorrelation&&) noexcept;
    ~ClockCorrelation();

    Result<UnixEpochClockAnchor> get_anchor(rodaq_clock_handle clock) const;

    rodaq_interface abi_interface() noexcept;

private:
    struct State;

    explicit ClockCorrelation(std::unique_ptr<State> state) noexcept;

    static rodaq_status_code get_unix_epoch_anchor(
        void* context, rodaq_clock_handle clock, rodaq_unix_epoch_clock_anchor* out_anchor,
        rodaq_status* out_status) noexcept;

    static const rodaq_runtime_clock_correlation kClockCorrelationVtable;

    std::unique_ptr<State> state_;
};

}  // namespace rodaq

#endif  // RODAQ_CLOCK_CLOCK_CORRELATION_HPP_
