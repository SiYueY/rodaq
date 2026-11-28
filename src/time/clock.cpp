#include "clock.hpp"

#include <cstdint>
#include <limits>
#include <time.h>
#include <utility>

namespace rodaq::time {
namespace {

constexpr std::uint64_t kBillion = 1000000000;
constexpr auto kSignedMax = std::numeric_limits<std::int64_t>::max();
constexpr auto kSignedMin = std::numeric_limits<std::int64_t>::min();

bool checked_add(std::int64_t left, std::int64_t right, std::int64_t& output) noexcept {
    if ((right > 0 && left > kSignedMax - right) || (right < 0 && left < kSignedMin - right)) {
        return false;
    }
    output = left + right;
    return true;
}

Result<ClockSample> sample_pair() {
    struct timespec before {};
    struct timespec realtime {};
    struct timespec after {};
    if (clock_gettime(CLOCK_MONOTONIC, &before) != 0 ||
        clock_gettime(CLOCK_REALTIME, &realtime) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &after) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    return pair_clock_readings(before, realtime, after);
}

}  // namespace

Result<ClockSample> pair_clock_readings(
    const timespec& before, const timespec& realtime, const timespec& after) {
    static_assert(sizeof(time_t) <= sizeof(std::int64_t), "Qualified Linux time_t range");
    for (const auto* reading : {&before, &realtime, &after}) {
        if (reading->tv_nsec < 0 || static_cast<std::uint64_t>(reading->tv_nsec) >= kBillion) {
            return Status(StatusCode::InvalidArgument, {});
        }
    }
    const auto monotonic = [](const timespec& reading) -> Result<std::uint64_t> {
        if (reading.tv_sec < 0) {
            return Status(StatusCode::InvalidArgument, {});
        }
        const auto seconds = static_cast<std::uint64_t>(reading.tv_sec);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        const auto nanos = static_cast<std::uint64_t>(reading.tv_nsec);
        if (seconds > maximum / kBillion ||
            (seconds == maximum / kBillion && nanos > maximum % kBillion)) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        return seconds * kBillion + nanos;
    };
    auto first = monotonic(before);
    auto last = monotonic(after);
    if (!first.ok()) {
        return first.status();
    }
    if (!last.ok()) {
        return last.status();
    }
    if (last.value() < first.value()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto seconds = static_cast<std::int64_t>(realtime.tv_sec);
    const auto adjusted = seconds < 0 ? seconds + 1 : seconds;
    if (adjusted < kSignedMin / static_cast<std::int64_t>(kBillion) ||
        adjusted > kSignedMax / static_cast<std::int64_t>(kBillion)) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto fraction = static_cast<std::int64_t>(realtime.tv_nsec) -
                          (seconds < 0 ? static_cast<std::int64_t>(kBillion) : 0);
    std::int64_t wall = 0;
    if (!checked_add(adjusted * static_cast<std::int64_t>(kBillion), fraction, wall)) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto span = last.value() - first.value();
    return ClockSample{first.value() + span / 2, wall, span / 2 + span % 2};
}

Result<ClockSample> sample_clocks() {
    auto best = sample_pair();
    if (!best.ok()) {
        return best.status();
    }
    for (unsigned int attempt = 0; attempt < 2; ++attempt) {
        auto candidate = sample_pair();
        if (!candidate.ok()) {
            return candidate.status();
        }
        if (candidate.value().error_bound_ns < best.value().error_bound_ns) {
            best = std::move(candidate);
        }
    }
    return best;
}

Result<std::uint64_t> project_recording_time(
    const PlaybackAnchor& anchor, std::uint64_t receive_monotonic_ns) {
    constexpr auto kUnsignedMax = std::numeric_limits<std::uint64_t>::max();
    if (receive_monotonic_ns >= anchor.monotonic_ns) {
        const auto offset = receive_monotonic_ns - anchor.monotonic_ns;
        if (anchor.realtime_ns < 0) {
            const auto magnitude = static_cast<std::uint64_t>(-(anchor.realtime_ns + 1)) + 1;
            if (offset < magnitude) {
                return Status(StatusCode::InvalidArgument, {});
            }
            return offset - magnitude;
        }
        const auto base = static_cast<std::uint64_t>(anchor.realtime_ns);
        if (offset > kUnsignedMax - base) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        return base + offset;
    }
    const auto back = anchor.monotonic_ns - receive_monotonic_ns;
    if (anchor.realtime_ns < 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto base = static_cast<std::uint64_t>(anchor.realtime_ns);
    if (back > base) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return base - back;
}

}  // namespace rodaq::time
