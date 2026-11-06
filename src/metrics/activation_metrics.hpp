#ifndef RODAQ_METRICS_ACTIVATION_METRICS_HPP_
#define RODAQ_METRICS_ACTIVATION_METRICS_HPP_

#include "metrics/latency_histogram.hpp"
#include "metrics/stats_accumulator.hpp"
#include "storage/storage_latency.hpp"

#include <rodaq/daq_stats.hpp>

#include <chrono>
#include <cstdint>
#include <memory>

namespace rodaq {

class ActivationMetrics final {
public:
    ActivationMetrics();

    void record_pre_rejected(bool gate_closed, bool timeout) noexcept;
    void record_admission_wait(std::int64_t duration_ns) noexcept;
    void record_storage_latency(StorageLatencyKind kind, std::int64_t duration_ns) noexcept;
    void record_accepted(std::uint64_t bytes) noexcept;
    void record_rejected() noexcept;
    void record_inflight_acquired(std::uint64_t bytes) noexcept;
    bool record_inflight_released(std::uint64_t bytes) noexcept;
    bool record_payload_released(std::uint64_t bytes) noexcept;
    bool record_operation_completed() noexcept;
    void record_persistence(std::uint32_t state) noexcept;

    void mark_stop_started(std::chrono::steady_clock::time_point started) noexcept;
    void record_stop_latency() noexcept;

    DAQStats snapshot(const LoggingStats& logging) const noexcept;

private:
    void fill_latency_stats(DAQStats& snapshot) const noexcept;

    StatsAccumulator stats_;
    std::unique_ptr<LatencyHistogram> admission_wait_;
    std::unique_ptr<LatencyHistogram> storage_handoff_;
    std::unique_ptr<LatencyHistogram> payload_consumed_;
    std::unique_ptr<LatencyHistogram> persistence_;
    std::unique_ptr<LatencyHistogram> stop_;
    std::chrono::steady_clock::time_point stop_started_{};
    bool stop_started_valid_ = false;
};

}  // namespace rodaq

#endif  // RODAQ_METRICS_ACTIVATION_METRICS_HPP_
