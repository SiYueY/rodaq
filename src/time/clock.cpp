#include "clock.hpp"

#include "../persistence/json.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <time.h>
#include <unistd.h>
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

bool checked_subtract(std::int64_t left, std::int64_t right, std::int64_t& output) noexcept {
    if ((right < 0 && left > kSignedMax + right) || (right > 0 && left < kSignedMin + right)) {
        return false;
    }
    output = left - right;
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
    // For negative seconds, normalize before multiplication so INT64_MIN's
    // valid fractional-second representation never underflows an intermediate.
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

Result<BootIdentity> parse_boot_identity(std::string_view kernel_uuid) {
    if (kernel_uuid.size() == 37 && kernel_uuid.back() == '\n') {
        kernel_uuid.remove_suffix(1);
    }
    if (kernel_uuid.size() != 36) {
        return Status(StatusCode::InvalidArgument, {});
    }
    bool nonzero = false;
    for (std::size_t index = 0; index < kernel_uuid.size(); ++index) {
        const auto value = kernel_uuid[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-') {
                return Status(StatusCode::InvalidArgument, {});
            }
        } else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return Status(StatusCode::InvalidArgument, {});
        } else {
            nonzero = nonzero || value != '0';
        }
    }
    if (!nonzero) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto digest = persistence::semantic_digest(
            persistence::Json{{"kernel_boot_uuid", kernel_uuid}}, "rodaq.system-boot-id.v1");
        if (!digest.ok()) {
            return digest.status();
        }
        rodaq_system_boot_id_t identity = 0;
        const auto* start = digest.value().data() + 7;
        const auto parsed = std::from_chars(start, start + 16, identity, 16);
        if (parsed.ec != std::errc{} || identity == 0) {
            return Status(StatusCode::DataLoss, {});
        }
        return BootIdentity{identity, std::string(kernel_uuid)};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<BootIdentity> read_boot_identity() {
    const auto file = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    std::array<char, 64> bytes{};
    std::size_t filled = 0;
    int failure = 0;
    while (filled < bytes.size()) {
        const auto count = read(file, bytes.data() + filled, bytes.size() - filled);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            failure = errno;
            break;
        }
        if (count == 0) {
            break;
        }
        filled += static_cast<std::size_t>(count);
    }
    close(file);
    if (failure != 0 || filled == bytes.size()) {
        return Status(StatusCode::Unavailable, {});
    }
    return parse_boot_identity({bytes.data(), filled});
}

Result<ClockSample> sample_clocks() {
    auto best = sample_pair();
    if (!best.ok()) {
        return best.status();
    }
    // Bounded sampling reduces scheduler-preemption error without a busy loop.
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

Result<CorrelationChange> compare_correlation(
    const ClockSample& previous, const ClockSample& current, std::uint64_t allowed_error_ns) {
    if (current.monotonic_ns < previous.monotonic_ns) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto monotonic_delta = current.monotonic_ns - previous.monotonic_ns;
    std::int64_t realtime_delta = 0;
    std::int64_t jump = 0;
    if (monotonic_delta > static_cast<std::uint64_t>(kSignedMax) ||
        !checked_subtract(current.realtime_ns, previous.realtime_ns, realtime_delta) ||
        !checked_subtract(realtime_delta, static_cast<std::int64_t>(monotonic_delta), jump)) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    if (previous.error_bound_ns > limit - allowed_error_ns ||
        current.error_bound_ns > limit - allowed_error_ns - previous.error_bound_ns) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto tolerance = allowed_error_ns + previous.error_bound_ns + current.error_bound_ns;
    const auto magnitude =
        jump < 0 ? static_cast<std::uint64_t>(-(jump + 1)) + 1 : static_cast<std::uint64_t>(jump);
    return CorrelationChange{magnitude > tolerance, jump};
}

Result<std::int64_t> project_playback_time(
    const PlaybackAnchor& anchor, rodaq_system_boot_id_t record_boot_id,
    std::uint64_t receive_monotonic_ns) {
    if (anchor.boot_id == 0 || record_boot_id != anchor.boot_id) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const bool offset_negative = receive_monotonic_ns < anchor.monotonic_ns;
    const auto offset = offset_negative ? anchor.monotonic_ns - receive_monotonic_ns
                                        : receive_monotonic_ns - anchor.monotonic_ns;
    bool negative = anchor.realtime_ns < 0;
    auto magnitude = negative ? static_cast<std::uint64_t>(-(anchor.realtime_ns + 1)) + 1
                              : static_cast<std::uint64_t>(anchor.realtime_ns);
    if (negative == offset_negative) {
        const auto limit = static_cast<std::uint64_t>(kSignedMax) + (negative ? 1u : 0u);
        if (offset > limit - magnitude) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        magnitude += offset;
    } else if (offset > magnitude) {
        magnitude = offset - magnitude;
        negative = offset_negative;
    } else {
        magnitude -= offset;
    }
    const auto limit = static_cast<std::uint64_t>(kSignedMax) + (negative ? 1u : 0u);
    if (magnitude > limit) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    if (negative && magnitude == static_cast<std::uint64_t>(kSignedMax) + 1) {
        return kSignedMin;
    }
    const auto value = static_cast<std::int64_t>(magnitude);
    return negative ? -value : value;
}

}  // namespace rodaq::time
