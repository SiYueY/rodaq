#include "benchmark_support.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace rodaq::benchmark_support {
namespace {

void BM_RuntimePipelineSmallRecordSteadyState(benchmark::State& state) {
    constexpr std::size_t kRecords = 100'000;
    constexpr std::size_t kPayloadBytes = 1024;
    const std::size_t capacity = record_capacity(kRecords);
    auto config = runtime_config(
        "runtime-pipeline-small-record", {PayloadClassConfig{kPayloadBytes, capacity}}, capacity,
        capacity * kPayloadBytes, 1, {acquisition(1, kRecords, kPayloadBytes, 0)});
    run_profile(state, config, kRecords, kRecords * kPayloadBytes, std::chrono::seconds(30));
}

}  // namespace
}  // namespace rodaq::benchmark_support

BENCHMARK(rodaq::benchmark_support::BM_RuntimePipelineSmallRecordSteadyState)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);

BENCHMARK_MAIN();
