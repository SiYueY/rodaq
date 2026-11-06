#include "benchmark_support.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace rodaq::benchmark_support {
namespace {

void bm_storage_slot_terminalization100k(benchmark::State& state) {
    constexpr std::size_t kRecords = 100'000;
    constexpr std::size_t kPayloadBytes = 16;
    const std::size_t capacity = record_capacity(kRecords);
    auto config = daq_config(
        "storage-slot-terminalization", {PayloadClassConfig{64, capacity}}, capacity,
        capacity * 64U, 1, {acquisition(1, kRecords, kPayloadBytes, 0)});
    run_profile(state, config, kRecords, kRecords * kPayloadBytes, std::chrono::seconds(30));
}

}  // namespace
}  // namespace rodaq::benchmark_support

BENCHMARK(rodaq::benchmark_support::bm_storage_slot_terminalization100k)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);

BENCHMARK_MAIN();
