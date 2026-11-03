#ifndef RODAQ_RUNTIME_LATENCY_HISTOGRAM_HPP_
#define RODAQ_RUNTIME_LATENCY_HISTOGRAM_HPP_

#include <rodaq/result.hpp>

#include <cstdint>
#include <memory>

namespace rodaq {

/// Thread-safe wrapper over the pinned HdrHistogram implementation.
class LatencyHistogram final {
public:
    static Result<std::unique_ptr<LatencyHistogram>> Create(
        std::int64_t lowest_discernible_value, std::int64_t highest_trackable_value,
        int significant_figures = 3);

    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;
    LatencyHistogram(LatencyHistogram&&) = delete;
    LatencyHistogram& operator=(LatencyHistogram&&) = delete;
    ~LatencyHistogram();

    bool Record(std::int64_t value) noexcept;
    std::uint64_t Count() const noexcept;
    std::int64_t Min() const noexcept;
    std::int64_t Max() const noexcept;
    std::int64_t Percentile(double percentile) const noexcept;
    std::int64_t P50() const noexcept { return Percentile(50.0); }
    std::int64_t P95() const noexcept { return Percentile(95.0); }
    std::int64_t P99() const noexcept { return Percentile(99.0); }

private:
    class Impl;

    explicit LatencyHistogram(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_LATENCY_HISTOGRAM_HPP_
