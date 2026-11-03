#include "rodaq/runtime/runtime_stats.hpp"

#include <atomic>
#include <cstdint>
#include <limits>

namespace rodaq {

namespace {

void add_saturated(std::atomic<std::uint64_t>& counter, std::uint64_t amount) noexcept {
    std::uint64_t current = counter.load();
    while (true) {
        if (current == std::numeric_limits<std::uint64_t>::max()) {
            return;
        }
        const std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max() - current;
        const std::uint64_t next =
            amount > remaining ? std::numeric_limits<std::uint64_t>::max() : current + amount;
        if (counter.compare_exchange_weak(current, next)) {
            return;
        }
    }
}

void update_high_water(std::atomic<std::uint64_t>& high_water, std::uint64_t observed) noexcept {
    std::uint64_t current = high_water.load();
    while (observed > current && !high_water.compare_exchange_weak(current, observed)) {
    }
}

}  // namespace

void RuntimeStats::RecordAccepted(std::uint64_t bytes) noexcept {
    add_saturated(accepted_records_, 1);
    add_saturated(accepted_bytes_, bytes);
}

void RuntimeStats::RecordRejected() noexcept { add_saturated(rejected_records_, 1); }

void RuntimeStats::RecordTimedOut() noexcept { add_saturated(timed_out_records_, 1); }

void RuntimeStats::RecordCancelled() noexcept { add_saturated(cancelled_records_, 1); }

void RuntimeStats::RecordGateClosed() noexcept { add_saturated(gate_closed_records_, 1); }

void RuntimeStats::RecordInflightAcquired(std::uint64_t bytes) noexcept {
    const std::uint64_t records = inflight_records_.fetch_add(1) + 1U;
    update_high_water(high_water_records_, records);
    const std::uint64_t total_bytes = inflight_bytes_.fetch_add(bytes) + bytes;
    update_high_water(high_water_bytes_, total_bytes);
}

bool RuntimeStats::RecordInflightReleased(std::uint64_t bytes) noexcept {
    std::uint64_t current_bytes = inflight_bytes_.load();
    if (bytes > current_bytes) {
        return false;
    }
    while (!inflight_bytes_.compare_exchange_weak(current_bytes, current_bytes - bytes)) {
        if (bytes > current_bytes) {
            return false;
        }
    }

    std::uint64_t current_records = inflight_records_.load();
    while (current_records != 0) {
        if (inflight_records_.compare_exchange_weak(current_records, current_records - 1U)) {
            return true;
        }
    }
    inflight_bytes_.fetch_add(bytes);
    return false;
}

RuntimeStatsSnapshot RuntimeStats::Snapshot() const noexcept {
    RuntimeStatsSnapshot snapshot;
    snapshot.accepted_records = accepted_records_.load();
    snapshot.rejected_records = rejected_records_.load();
    snapshot.timed_out_records = timed_out_records_.load();
    snapshot.cancelled_records = cancelled_records_.load();
    snapshot.gate_closed_records = gate_closed_records_.load();
    snapshot.accepted_bytes = accepted_bytes_.load();
    snapshot.inflight_records = inflight_records_.load();
    snapshot.inflight_bytes = inflight_bytes_.load();
    snapshot.high_water_records = high_water_records_.load();
    snapshot.high_water_bytes = high_water_bytes_.load();
    return snapshot;
}

}  // namespace rodaq
