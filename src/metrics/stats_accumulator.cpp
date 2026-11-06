#include "metrics/stats_accumulator.hpp"

#include <rodaq/plugin/abi/storage.h>

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

void StatsAccumulator::record_accepted(std::uint64_t bytes) noexcept {
    add_saturated(accepted_records_, 1);
    add_saturated(accepted_bytes_, bytes);
}

void StatsAccumulator::record_rejected() noexcept { add_saturated(rejected_records_, 1); }

void StatsAccumulator::record_timed_out() noexcept { add_saturated(timed_out_records_, 1); }

void StatsAccumulator::record_cancelled() noexcept { add_saturated(cancelled_records_, 1); }

void StatsAccumulator::record_gate_closed() noexcept { add_saturated(gate_closed_records_, 1); }

void StatsAccumulator::record_inflight_acquired(std::uint64_t bytes) noexcept {
    const std::uint64_t records = inflight_records_.fetch_add(1) + 1U;
    update_high_water(high_water_records_, records);
    const std::uint64_t total_bytes = inflight_bytes_.fetch_add(bytes) + bytes;
    update_high_water(high_water_bytes_, total_bytes);
}

bool StatsAccumulator::record_inflight_released(std::uint64_t bytes) noexcept {
    if (!record_payload_released(bytes)) {
        return false;
    }
    return record_operation_completed();
}

bool StatsAccumulator::record_payload_released(std::uint64_t bytes) noexcept {
    std::uint64_t current_bytes = inflight_bytes_.load();
    if (bytes > current_bytes) {
        return false;
    }
    while (!inflight_bytes_.compare_exchange_weak(current_bytes, current_bytes - bytes)) {
        if (bytes > current_bytes) {
            return false;
        }
    }
    return true;
}

bool StatsAccumulator::record_operation_completed() noexcept {
    std::uint64_t current_records = inflight_records_.load();
    while (current_records != 0) {
        if (inflight_records_.compare_exchange_weak(current_records, current_records - 1U)) {
            return true;
        }
    }
    return false;
}

void StatsAccumulator::record_persistence(std::uint32_t state) noexcept {
    if (state == RODAQ_STORAGE_PERSISTENCE_SUCCEEDED) {
        add_saturated(persistence_succeeded_, 1);
    } else if (state == RODAQ_STORAGE_PERSISTENCE_FAILED) {
        add_saturated(persistence_failed_, 1);
    } else {
        add_saturated(persistence_unknown_, 1);
    }
}

DAQStats StatsAccumulator::snapshot() const noexcept {
    DAQStats snapshot;
    snapshot.persistence_succeeded_records = persistence_succeeded_.load();
    snapshot.persistence_failed_records = persistence_failed_.load();
    snapshot.persistence_unknown_records = persistence_unknown_.load();
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
