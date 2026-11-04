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
constexpr std::size_t kJointCount = 100;
constexpr std::size_t kJointRateHz = 1'000;
constexpr std::size_t kMeasurementSeconds = 1;
constexpr std::size_t kEncodedFrameBytes = 200U * 1024U;
constexpr std::size_t kRawRgb8FrameBytes = 1280U * 720U * 3U;
constexpr std::size_t kAggregatedJointBytes = 100U * 100U * sizeof(double);
constexpr std::size_t kPerJointBytes = 100U * sizeof(double);

void run_encoded_aggregated(benchmark::State& state) {
    const std::size_t camera_capacity = record_capacity(kCameraCount * kCameraFps);
    const std::size_t joint_capacity = record_capacity(kJointRateHz);
    const std::size_t operation_capacity = std::max(camera_capacity, joint_capacity);
    const std::uint64_t expected_records =
        kCameraCount * kCameraFps * kMeasurementSeconds + kJointRateHz * kMeasurementSeconds;
    const std::uint64_t expected_bytes =
        kCameraCount * kCameraFps * kMeasurementSeconds * kEncodedFrameBytes +
        kJointRateHz * kMeasurementSeconds * kAggregatedJointBytes;
    auto config = runtime_config(
        "mixed-encoded-aggregated",
        {PayloadClassConfig{kAggregatedJointBytes, joint_capacity},
         PayloadClassConfig{kEncodedFrameBytes, camera_capacity}},
        operation_capacity, 128U * 1024U * 1024U, kCameraCount + 1,
        {acquisition(
             kCameraCount, kCameraFps * kMeasurementSeconds, kEncodedFrameBytes,
             1'000'000'000U / kCameraFps),
         acquisition(
             1, kJointRateHz * kMeasurementSeconds, kAggregatedJointBytes,
             1'000'000'000U / kJointRateHz)});
    run_profile(state, config, expected_records, expected_bytes, std::chrono::seconds(30), 1.10);
}

void run_raw_per_joint(benchmark::State& state) {
    const std::size_t camera_capacity = record_capacity(kCameraCount * kCameraFps);
    const std::size_t joint_capacity = record_capacity(kJointCount * kJointRateHz);
    const std::size_t operation_capacity = std::max(camera_capacity, joint_capacity);
    const std::uint64_t camera_records = kCameraCount * kCameraFps * kMeasurementSeconds;
    const std::uint64_t joint_records = kJointCount * kJointRateHz * kMeasurementSeconds;
    const std::uint64_t expected_records = camera_records + joint_records;
    const std::uint64_t expected_bytes =
        camera_records * kRawRgb8FrameBytes + joint_records * kPerJointBytes;
    const std::uint64_t physical_camera_bytes =
        static_cast<std::uint64_t>(camera_capacity) * kRawRgb8FrameBytes;
    const std::uint64_t physical_joint_bytes = static_cast<std::uint64_t>(joint_capacity) * 1024U;
    auto config = runtime_config(
        "mixed-raw-per-joint",
        {PayloadClassConfig{1024, joint_capacity},
         PayloadClassConfig{kRawRgb8FrameBytes, camera_capacity}},
        operation_capacity, (physical_camera_bytes + physical_joint_bytes) * 2U,
        kCameraCount + kJointCount,
        {acquisition(
             kCameraCount, kCameraFps * kMeasurementSeconds, kRawRgb8FrameBytes,
             1'000'000'000U / kCameraFps),
         acquisition(
             kJointCount, kJointRateHz * kMeasurementSeconds, kPerJointBytes,
             1'000'000'000U / kJointRateHz)});
    run_profile(state, config, expected_records, expected_bytes, std::chrono::seconds(45), 1.10);
}

}  // namespace
}  // namespace rodaq::benchmark_support

BENCHMARK(rodaq::benchmark_support::run_encoded_aggregated)
    ->Iterations(1)
    ->Unit(benchmark::kSecond);
BENCHMARK(rodaq::benchmark_support::run_raw_per_joint)->Iterations(1)->Unit(benchmark::kSecond);

BENCHMARK_MAIN();
