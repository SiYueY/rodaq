#ifndef RODAQ_RUNTIME_RUNTIME_STATS_HPP_
#define RODAQ_RUNTIME_RUNTIME_STATS_HPP_

#include <atomic>
#include <cstdint>

namespace rodaq {

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

    void RecordAccepted(std::uint64_t bytes) noexcept;
    void RecordRejected() noexcept;
    void RecordTimedOut() noexcept;
    void RecordCancelled() noexcept;
    void RecordGateClosed() noexcept;

    void RecordInflightAcquired(std::uint64_t bytes) noexcept;
    bool RecordInflightReleased(std::uint64_t bytes) noexcept;
    bool RecordPayloadReleased(std::uint64_t bytes) noexcept;
    bool RecordOperationCompleted() noexcept;

    RuntimeStatsSnapshot Snapshot() const noexcept;

private:
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
