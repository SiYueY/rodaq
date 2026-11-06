#ifndef RODAQ_METRICS_STATS_ACCUMULATOR_HPP_
#define RODAQ_METRICS_STATS_ACCUMULATOR_HPP_

#include <rodaq/daq_stats.hpp>

#include <atomic>
#include <cstdint>

namespace rodaq {

/// Low-cost eventually-consistent DAQ counters.
///
/// Individual counters are atomic. A snapshot is not a single atomic cut
/// across all fields.
class StatsAccumulator final {
public:
    StatsAccumulator() = default;
    StatsAccumulator(const StatsAccumulator&) = delete;
    StatsAccumulator& operator=(const StatsAccumulator&) = delete;
    StatsAccumulator(StatsAccumulator&&) = delete;
    StatsAccumulator& operator=(StatsAccumulator&&) = delete;

    void record_accepted(std::uint64_t bytes) noexcept;
    void record_rejected() noexcept;
    void record_timed_out() noexcept;
    void record_cancelled() noexcept;
    void record_gate_closed() noexcept;

    void record_inflight_acquired(std::uint64_t bytes) noexcept;
    bool record_inflight_released(std::uint64_t bytes) noexcept;
    bool record_payload_released(std::uint64_t bytes) noexcept;
    bool record_operation_completed() noexcept;

    void record_persistence(std::uint32_t state) noexcept;

    DAQStats snapshot() const noexcept;

private:
    std::atomic<std::uint64_t> persistence_succeeded_{0};
    std::atomic<std::uint64_t> persistence_failed_{0};
    std::atomic<std::uint64_t> persistence_unknown_{0};
    std::atomic<std::uint64_t> accepted_records_{0};
    std::atomic<std::uint64_t> rejected_records_{0};
    std::atomic<std::uint64_t> timed_out_records_{0};
    std::atomic<std::uint64_t> cancelled_records_{0};
    std::atomic<std::uint64_t> gate_closed_records_{0};
    std::atomic<std::uint64_t> accepted_bytes_{0};
    std::atomic<std::uint64_t> inflight_records_{0};
    std::atomic<std::uint64_t> inflight_bytes_{0};
    std::atomic<std::uint64_t> high_water_records_{0};
    std::atomic<std::uint64_t> high_water_bytes_{0};
};

}  // namespace rodaq

#endif  // RODAQ_METRICS_STATS_ACCUMULATOR_HPP_
