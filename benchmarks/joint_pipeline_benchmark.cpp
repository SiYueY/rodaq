#include "benchmark_support.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace rodaq::benchmark_support {
namespace {

constexpr std::size_t kJointCount = 100;
constexpr std::size_t kParametersPerJoint = 100;
constexpr std::size_t kJointRateHz = 1'000;
constexpr std::size_t kMeasurementSeconds = 1;
constexpr std::size_t kPerJointBytes = kParametersPerJoint * sizeof(double);
constexpr std::size_t kAggregatedBytes = kJointCount * kParametersPerJoint * sizeof(double);

void run_joint_profile(
    benchmark::State& state, std::size_t streams, std::size_t payload_bytes, std::string name,
    std::size_t block_bytes) {
    const std::uint64_t peak_records_per_second = streams * kJointRateHz;
    const std::size_t capacity = record_capacity(peak_records_per_second);
    const std::uint64_t expected_records = peak_records_per_second * kMeasurementSeconds;
    const std::uint64_t expected_bytes = expected_records * payload_bytes;
    auto config = daq_config(
        std::move(name), {PayloadClassConfig{block_bytes, capacity}}, capacity,
        capacity * block_bytes, streams,
        {acquisition(
            streams, kJointRateHz * kMeasurementSeconds, payload_bytes,
            1'000'000'000U / kJointRateHz)});
    run_profile(state, config, expected_records, expected_bytes, std::chrono::seconds(30), 1.10);
}

void bm_per_joint100_streams1k_hz(benchmark::State& state) {
    run_joint_profile(state, kJointCount, kPerJointBytes, "per-joint-100khz", 1024);
}

void bm_aggregated_robot_state1k_hz(benchmark::State& state) {
    run_joint_profile(state, 1, kAggregatedBytes, "aggregated-joint-1khz", 96U * 1024U);
}

}  // namespace
}  // namespace rodaq::benchmark_support

BENCHMARK(rodaq::benchmark_support::bm_per_joint100_streams1k_hz)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);
BENCHMARK(rodaq::benchmark_support::bm_aggregated_robot_state1k_hz)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);

BENCHMARK_MAIN();
