#include <rodaq/runtime/payload_pool.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

constexpr std::size_t kBlockCount = 16;

void fixed_block_acquire_release(benchmark::State& state, std::size_t bytes) {
    PayloadPoolConfig config;
    config.classes.push_back(PayloadClassConfig{bytes, kBlockCount});
    auto pool = PayloadPool::Create(config);
    if (!pool.has_value()) {
        state.SkipWithError("failed to create PayloadPool");
        return;
    }

    for (auto iteration : state) {
        static_cast<void>(iteration);
        auto reservation = pool.value().TryReserve(bytes);
        if (!reservation.has_value()) {
            state.SkipWithError("PayloadPool reserve failed");
            return;
        }
        benchmark::DoNotOptimize(reservation.value());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes));
}

void fixed_block_acquire_copy_release(benchmark::State& state, std::size_t bytes) {
    PayloadPoolConfig config;
    config.classes.push_back(PayloadClassConfig{bytes, kBlockCount});
    auto pool = PayloadPool::Create(config);
    if (!pool.has_value()) {
        state.SkipWithError("failed to create PayloadPool");
        return;
    }
    std::vector<std::uint8_t> source(bytes, std::uint8_t{1});

    for (auto iteration : state) {
        static_cast<void>(iteration);
        auto reservation = pool.value().TryReserve(bytes);
        if (!reservation.has_value()) {
            state.SkipWithError("PayloadPool reserve failed");
            return;
        }
        auto payload = pool.value().Materialize(
            std::move(reservation.value()), PayloadView(source.data(), source.size()));
        if (!payload.has_value()) {
            state.SkipWithError("PayloadPool copy acquire failed");
            return;
        }
        benchmark::DoNotOptimize(payload.value().View().Data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes));
}

void BM_PayloadPoolAcquireRelease(benchmark::State& state, std::size_t bytes) {
    fixed_block_acquire_release(state, bytes);
}

void BM_PayloadPoolAcquireCopyRelease(benchmark::State& state, std::size_t bytes) {
    fixed_block_acquire_copy_release(state, bytes);
}

BENCHMARK_CAPTURE(BM_PayloadPoolAcquireRelease, 1KiB, std::size_t{1024})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PayloadPoolAcquireRelease, 64KiB, std::size_t{64} * 1024)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PayloadPoolAcquireRelease, 1MiB, std::size_t{1024} * 1024)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PayloadPoolAcquireRelease, 2_7MiB, std::size_t{2700} * 1024)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_CAPTURE(BM_PayloadPoolAcquireCopyRelease, 1KiB, std::size_t{1024})
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PayloadPoolAcquireCopyRelease, 64KiB, std::size_t{64} * 1024)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PayloadPoolAcquireCopyRelease, 1MiB, std::size_t{1024} * 1024)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_CAPTURE(BM_PayloadPoolAcquireCopyRelease, 2_7MiB, std::size_t{2700} * 1024)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace rodaq

BENCHMARK_MAIN();
