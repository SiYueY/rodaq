#include "benchmark_support.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rodaq::benchmark_support {
namespace {

constexpr std::size_t kCameraCount = 10;
constexpr std::size_t kCameraFps = 30;
constexpr std::size_t kMeasurementSeconds = 1;
constexpr std::size_t kRecordsPerStream = kCameraFps * kMeasurementSeconds;
constexpr std::size_t kEncodedFrameBytes = 200U * 1024U;
constexpr std::size_t kRawRgb8FrameBytes = 1280U * 720U * 3U;

void run_camera_profile(benchmark::State& state, std::size_t frame_bytes, std::string name) {
    const std::size_t capacity = record_capacity(kCameraCount * kCameraFps);
    const std::uint64_t expected_records = kCameraCount * kRecordsPerStream;
    const std::uint64_t expected_bytes = expected_records * frame_bytes;
    auto config = daq_config(
        std::move(name), {PayloadClassConfig{frame_bytes, capacity}}, capacity,
        capacity * frame_bytes, kCameraCount,
        {acquisition(kCameraCount, kRecordsPerStream, frame_bytes, 1'000'000'000U / kCameraFps)});
    run_profile(state, config, expected_records, expected_bytes, std::chrono::seconds(30), 1.10);
}

void bm_encoded_camera10x720p30(benchmark::State& state) {
    run_camera_profile(state, kEncodedFrameBytes, "camera-encoded-capacity");
}

void bm_raw_camera10x720p30(benchmark::State& state) {
    run_camera_profile(state, kRawRgb8FrameBytes, "camera-raw-capacity");
}

}  // namespace
}  // namespace rodaq::benchmark_support

BENCHMARK(rodaq::benchmark_support::bm_encoded_camera10x720p30)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);
BENCHMARK(rodaq::benchmark_support::bm_raw_camera10x720p30)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);

BENCHMARK_MAIN();
