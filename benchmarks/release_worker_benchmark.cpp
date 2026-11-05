#include "benchmark_support.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rodaq::benchmark_support {
namespace {

void bm_adopt_release_workers(benchmark::State& state) {
    constexpr std::size_t kRecords = 100'000;
    constexpr std::size_t kPayloadBytes = 800;
    const std::size_t capacity = record_capacity(kRecords);
    auto config = runtime_config(
        "adopt-release-workers", {PayloadClassConfig{1024, 1}}, capacity, capacity * kPayloadBytes,
        1, {acquisition(1, kRecords, kPayloadBytes, 0, "adopt")});
    config.resources.payload_release_workers = static_cast<std::size_t>(state.range(0));
    run_profile(state, config, kRecords, kRecords * kPayloadBytes, std::chrono::seconds(30));
}

}  // namespace
}  // namespace rodaq::benchmark_support

BENCHMARK(rodaq::benchmark_support::bm_adopt_release_workers)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);

BENCHMARK_MAIN();
