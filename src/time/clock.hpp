#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

#include <rodaq/canonical/c/types.h>
#include <rodaq/result.hpp>

namespace rodaq::time {

struct BootIdentity {
    rodaq_system_boot_id_t id{0};
    std::string kernel_uuid;
};

struct ClockSample {
    std::uint64_t monotonic_ns{0};
    std::int64_t realtime_ns{0};
    std::uint64_t error_bound_ns{0};
};

struct PlaybackAnchor {
    rodaq_system_boot_id_t boot_id{0};
    std::uint64_t monotonic_ns{0};
    std::int64_t realtime_ns{0};
};

struct ClockJump {
    rodaq_clock_epoch_id_t previous_epoch_id{0};
    std::int64_t delta_ns{0};
    std::uint64_t detected_monotonic_ns{0};
};

enum class ClockEpochOrigin { Boot, AuthorityRestart, RealtimeJump };

struct ClockSnapshot {
    rodaq_system_boot_id_t boot_id{0};
    rodaq_clock_epoch_id_t epoch_id{0};
    ClockSample sample;
    ClockEpochOrigin origin{ClockEpochOrigin::Boot};
    std::optional<ClockJump> jump;
};

struct CorrelationChange {
    bool discontinuity{false};
    std::int64_t delta_ns{0};
};

Result<BootIdentity> parse_boot_identity(std::string_view kernel_uuid);
Result<BootIdentity> read_boot_identity();
Result<ClockSample> sample_clocks();
Result<ClockSample> pair_clock_readings(
    const timespec& before, const timespec& realtime, const timespec& after);
Result<CorrelationChange> compare_correlation(
    const ClockSample& previous, const ClockSample& current, std::uint64_t allowed_error_ns);
Result<std::int64_t> project_playback_time(
    const PlaybackAnchor& anchor, rodaq_system_boot_id_t record_boot_id,
    std::uint64_t receive_monotonic_ns);

}  // namespace rodaq::time
