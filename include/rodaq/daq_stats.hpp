#ifndef RODAQ_DAQ_STATS_HPP_
#define RODAQ_DAQ_STATS_HPP_

#include <rodaq/logging_config.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rodaq {

struct PayloadClassStats {
    std::size_t max_payload_bytes = 0;
    std::size_t block_count = 0;
    std::size_t used_blocks = 0;
    std::size_t available_blocks = 0;
    std::size_t high_water_blocks = 0;
    std::uint64_t current_physical_bytes = 0;
    std::uint64_t high_water_physical_bytes = 0;
    std::uint64_t exhaustion_count = 0;
};

struct PayloadStats {
    std::vector<PayloadClassStats> classes;
};

struct LatencyStats {
    std::uint64_t count = 0;
    std::int64_t minimum_ns = 0;
    std::int64_t maximum_ns = 0;
    std::int64_t p50_ns = 0;
    std::int64_t p95_ns = 0;
    std::int64_t p99_ns = 0;
    std::int64_t p999_ns = 0;
};

struct DAQLatencyStats {
    LatencyStats admission_wait;
    LatencyStats storage_handoff;
    LatencyStats payload_consumed;
    LatencyStats persistence;
    LatencyStats stop;
};

struct DAQStats {
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
    DAQLatencyStats latencies;
    LoggingStats logging;
};

}  // namespace rodaq

#endif  // RODAQ_DAQ_STATS_HPP_
