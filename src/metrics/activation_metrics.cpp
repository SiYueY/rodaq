#include "metrics/activation_metrics.hpp"

#include <stdexcept>
#include <string>

namespace rodaq {
namespace {

std::unique_ptr<LatencyHistogram> make_latency_histogram() {
    auto result = LatencyHistogram::create(1, 60LL * 1'000'000'000LL, 3);
    if (!result.ok()) {
        throw std::runtime_error(std::string(result.status().message()));
    }
    return std::move(result.value());
}

LatencyStats latency_snapshot(const LatencyHistogram& histogram) noexcept {
    LatencyStats snapshot;
    snapshot.count = histogram.count();
    if (snapshot.count == 0) {
        return snapshot;
    }
    snapshot.minimum_ns = histogram.min();
    snapshot.maximum_ns = histogram.max();
    snapshot.p50_ns = histogram.p50();
    snapshot.p95_ns = histogram.p95();
    snapshot.p99_ns = histogram.p99();
    snapshot.p999_ns = histogram.percentile(99.9);
    return snapshot;
}

}  // namespace

ActivationMetrics::ActivationMetrics()
: admission_wait_(make_latency_histogram()),
  storage_handoff_(make_latency_histogram()),
  payload_consumed_(make_latency_histogram()),
  persistence_(make_latency_histogram()),
  stop_(make_latency_histogram()) {}

void ActivationMetrics::record_pre_rejected(bool gate_closed, bool timeout) noexcept {
    if (gate_closed) {
        stats_.record_gate_closed();
    } else if (timeout) {
        stats_.record_timed_out();
    } else {
        stats_.record_rejected();
    }
}

void ActivationMetrics::record_admission_wait(std::int64_t duration_ns) noexcept {
    if (duration_ns >= 0) {
        static_cast<void>(admission_wait_->record(duration_ns));
    }
}

void ActivationMetrics::record_storage_latency(
    StorageLatencyKind kind, std::int64_t duration_ns) noexcept {
    LatencyHistogram* histogram = nullptr;
    switch (kind) {
        case StorageLatencyKind::Handoff:
            histogram = storage_handoff_.get();
            break;
        case StorageLatencyKind::PayloadConsumed:
            histogram = payload_consumed_.get();
            break;
        case StorageLatencyKind::PersistenceCompleted:
            histogram = persistence_.get();
            break;
    }
    if (duration_ns >= 0) {
        static_cast<void>(histogram->record(duration_ns));
    }
}

void ActivationMetrics::record_accepted(std::uint64_t bytes) noexcept {
    stats_.record_accepted(bytes);
}

void ActivationMetrics::record_rejected() noexcept { stats_.record_rejected(); }

void ActivationMetrics::record_inflight_acquired(std::uint64_t bytes) noexcept {
    stats_.record_inflight_acquired(bytes);
}

bool ActivationMetrics::record_inflight_released(std::uint64_t bytes) noexcept {
    return stats_.record_inflight_released(bytes);
}

bool ActivationMetrics::record_payload_released(std::uint64_t bytes) noexcept {
    return stats_.record_payload_released(bytes);
}

bool ActivationMetrics::record_operation_completed() noexcept {
    return stats_.record_operation_completed();
}

void ActivationMetrics::record_persistence(std::uint32_t state) noexcept {
    stats_.record_persistence(state);
}

void ActivationMetrics::mark_stop_started(std::chrono::steady_clock::time_point started) noexcept {
    stop_started_ = started;
    stop_started_valid_ = true;
}

void ActivationMetrics::record_stop_latency() noexcept {
    if (!stop_started_valid_) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - stop_started_)
                             .count();
    if (elapsed >= 0) {
        static_cast<void>(stop_->record(elapsed));
    }
    stop_started_valid_ = false;
}

void ActivationMetrics::fill_latency_stats(DAQStats& snapshot) const noexcept {
    snapshot.latencies.admission_wait = latency_snapshot(*admission_wait_);
    snapshot.latencies.storage_handoff = latency_snapshot(*storage_handoff_);
    snapshot.latencies.payload_consumed = latency_snapshot(*payload_consumed_);
    snapshot.latencies.persistence = latency_snapshot(*persistence_);
    snapshot.latencies.stop = latency_snapshot(*stop_);
}

DAQStats ActivationMetrics::snapshot(const LoggingStats& logging) const noexcept {
    DAQStats snapshot = stats_.snapshot();
    snapshot.logging = logging;
    fill_latency_stats(snapshot);
    return snapshot;
}

}  // namespace rodaq
