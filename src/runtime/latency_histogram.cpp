#include "rodaq/runtime/latency_histogram.hpp"

#include <hdr/hdr_histogram.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>

namespace rodaq {

class LatencyHistogram::Impl final {
public:
    explicit Impl(hdr_histogram* histogram) noexcept : histogram_(histogram) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        if (histogram_ != nullptr) {
            hdr_close(histogram_);
        }
    }

    bool record(std::int64_t sample) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return hdr_record_value(histogram_, sample);
    }

    std::uint64_t count() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<std::uint64_t>(hdr_total_count(histogram_));
    }

    std::int64_t min() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return hdr_min(histogram_);
    }

    std::int64_t max() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return hdr_max(histogram_);
    }

    std::int64_t percentile(double percentile) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return hdr_value_at_percentile(histogram_, percentile);
    }

private:
    hdr_histogram* histogram_ = nullptr;
    mutable std::mutex mutex_;
};

Result<std::unique_ptr<LatencyHistogram>> LatencyHistogram::create(
    std::int64_t lowest_discernible_value, std::int64_t highest_trackable_value,
    int significant_figures) {
    if (lowest_discernible_value < 1 || highest_trackable_value < lowest_discernible_value ||
        significant_figures < 1 || significant_figures > 5) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "latency histogram configuration is invalid");
    }

    hdr_histogram* histogram = nullptr;
    const int status = hdr_init(
        lowest_discernible_value, highest_trackable_value, significant_figures, &histogram);
    if (status != 0 || histogram == nullptr) {
        if (histogram != nullptr) {
            hdr_close(histogram);
        }
        return rodaq::Status(
            status == ENOMEM ? StatusCode::ResourceExhausted : StatusCode::InvalidArgument,
            "failed to initialize latency histogram");
    }

    try {
        auto impl = std::make_unique<Impl>(histogram);
        return std::unique_ptr<LatencyHistogram>(new LatencyHistogram(std::move(impl)));
    } catch (const std::bad_alloc&) {
        hdr_close(histogram);
        return rodaq::Status(
            StatusCode::ResourceExhausted, "failed to allocate latency histogram wrapper");
    }
}

LatencyHistogram::LatencyHistogram(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

LatencyHistogram::~LatencyHistogram() = default;

bool LatencyHistogram::record(std::int64_t sample) noexcept { return impl_->record(sample); }

std::uint64_t LatencyHistogram::count() const noexcept { return impl_->count(); }

std::int64_t LatencyHistogram::min() const noexcept { return impl_->min(); }

std::int64_t LatencyHistogram::max() const noexcept { return impl_->max(); }

std::int64_t LatencyHistogram::percentile(double percentile) const noexcept {
    return impl_->percentile(percentile);
}

}  // namespace rodaq
