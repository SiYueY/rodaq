#include <rodaq/runtime/payload_pool.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

void BM_MixedSizeClassReserveCopyRelease(benchmark::State& state) {
    PayloadPoolConfig config;
    config.classes = {
        PayloadClassConfig{1024, 1024},
        PayloadClassConfig{96U * 1024U, 256},
        PayloadClassConfig{4U * 1024U * 1024U, 32},
    };
    auto pool = PayloadPool::Create(config);
    if (!pool.has_value()) {
        state.SkipWithError("failed to create multi-class PayloadPool");
        return;
    }

    const std::vector<std::size_t> request_sizes = {
        800,
        80'000,
        1280U * 720U * 3U,
    };
    std::vector<std::vector<std::uint8_t>> sources;
    sources.reserve(request_sizes.size());
    for (const std::size_t size : request_sizes) {
        sources.emplace_back(size, std::uint8_t{1});
    }

    std::size_t selected = 0;
    for (auto iteration : state) {
        static_cast<void>(iteration);
        const std::size_t index = selected++ % request_sizes.size();
        auto reservation = pool.value().TryReserve(request_sizes[index]);
        if (!reservation.has_value()) {
            state.SkipWithError("mixed size-class reserve failed");
            return;
        }
        auto payload = pool.value().Materialize(
            std::move(reservation.value()),
            PayloadView(sources[index].data(), sources[index].size()));
        if (!payload.has_value()) {
            state.SkipWithError("mixed size-class materialization failed");
            return;
        }
        benchmark::DoNotOptimize(payload.value().View().Data());
    }
    state.SetBytesProcessed(
        static_cast<std::int64_t>(state.iterations()) *
        static_cast<std::int64_t>((800U + 80'000U + 1280U * 720U * 3U) / 3U));
}

}  // namespace
}  // namespace rodaq

BENCHMARK(rodaq::BM_MixedSizeClassReserveCopyRelease)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
