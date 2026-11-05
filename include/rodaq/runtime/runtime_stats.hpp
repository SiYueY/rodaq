#ifndef RODAQ_RUNTIME_RUNTIME_STATS_HPP_
#define RODAQ_RUNTIME_RUNTIME_STATS_HPP_

#include <rodaq/runtime/logging.hpp>

#include <atomic>
#include <cstdint>

namespace rodaq {

struct LatencySnapshot {
    std::uint64_t count = 0;
    std::int64_t minimum_ns = 0;
    std::int64_t maximum_ns = 0;
    std::int64_t p50_ns = 0;
    std::int64_t p95_ns = 0;
    std::int64_t p99_ns = 0;
    std::int64_t p999_ns = 0;
};

struct RuntimeLatencySnapshot {
    LatencySnapshot admission_wait;
    LatencySnapshot storage_handoff;
    LatencySnapshot payload_consumed;
    LatencySnapshot persistence;
    LatencySnapshot stop;
};

struct RuntimeStatsSnapshot {
    std::uint64_t accepted_records = 0;
    std::uint64_t rejected_records = 0;
    std::uint64_t timed_out_records = 0;
    std::uint64_t cancelled_records = 0;
    std::uint64_t gate_closed_records = 0;
    std::uint64_t accepted_bytes = 0;
    std::uint64_t inflight_records = 0;
    std::uint64_t inflight_bytes = 0;
    std::uint64_t high_water_records = 0;
    std::uint64_t high_water_bytes = 0;
    std::uint64_t persistence_succeeded_records = 0;
    std::uint64_t persistence_failed_records = 0;
    std::uint64_t persistence_unknown_records = 0;
    RuntimeLatencySnapshot latencies;
    LoggingStats logging;
};

/// Low-cost Eventually-consistent Runtime counters.
///
/// Individual counters are atomic. A snapshot is not a single atomic cut
/// across all fields.
class RuntimeStats final {
public:
    RuntimeStats() = default;
    RuntimeStats(const RuntimeStats&) = delete;
    RuntimeStats& operator=(const RuntimeStats&) = delete;
    RuntimeStats(RuntimeStats&&) = delete;
    RuntimeStats& operator=(RuntimeStats&&) = delete;

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

    RuntimeStatsSnapshot snapshot() const noexcept;

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

#endif  // RODAQ_RUNTIME_RUNTIME_STATS_HPP_
