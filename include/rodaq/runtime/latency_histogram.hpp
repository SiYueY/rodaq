#ifndef RODAQ_RUNTIME_LATENCY_HISTOGRAM_HPP_
#define RODAQ_RUNTIME_LATENCY_HISTOGRAM_HPP_

#include <rodaq/result.hpp>

#include <cstdint>
#include <memory>

namespace rodaq {

/// Thread-safe wrapper over the pinned HdrHistogram implementation.
class LatencyHistogram final {
public:
    static Result<std::unique_ptr<LatencyHistogram>> create(
        std::int64_t lowest_discernible_value, std::int64_t highest_trackable_value,
        int significant_figures = 3);

    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;
    LatencyHistogram(LatencyHistogram&&) = delete;
    LatencyHistogram& operator=(LatencyHistogram&&) = delete;
    ~LatencyHistogram();

    bool record(std::int64_t sample) noexcept;
    std::uint64_t count() const noexcept;
    std::int64_t min() const noexcept;
    std::int64_t max() const noexcept;
    std::int64_t percentile(double percentile) const noexcept;
    std::int64_t p50() const noexcept { return percentile(50.0); }
    std::int64_t p95() const noexcept { return percentile(95.0); }
    std::int64_t p99() const noexcept { return percentile(99.0); }

private:
    class Impl;

    explicit LatencyHistogram(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_LATENCY_HISTOGRAM_HPP_
