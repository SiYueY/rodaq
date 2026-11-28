#pragma once

#include <cstdint>
#include <ctime>

#include <rodaq/result.hpp>

namespace rodaq::time {

struct ClockSample {
    std::uint64_t monotonic_ns{0};
    std::int64_t realtime_ns{0};
    std::uint64_t error_bound_ns{0};
};

// Per-Recording-Instance playback anchor sampled once during Prepare.
struct PlaybackAnchor {
    std::uint64_t monotonic_ns{0};
    std::int64_t realtime_ns{0};
};

Result<ClockSample> sample_clocks();
Result<ClockSample> pair_clock_readings(
    const timespec& before, const timespec& realtime, const timespec& after);
Result<std::uint64_t> project_recording_time(
    const PlaybackAnchor& anchor, std::uint64_t receive_monotonic_ns);

}  // namespace rodaq::time
