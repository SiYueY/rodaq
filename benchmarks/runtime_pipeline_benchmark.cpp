#include "plugin/config_view.hpp"
#include "runtime/runtime.hpp"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq::runtime {
namespace {

using Clock = std::chrono::steady_clock;

std::filesystem::path required_path(const char* environment_name) {
    const char* value = std::getenv(environment_name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("Missing benchmark environment: ") + environment_name);
    }
    return std::filesystem::path(value);
}

plugin::ConfigTree config(std::vector<plugin::ConfigTree::MapEntry> entries) {
    auto result = plugin::ConfigTree::Map(std::move(entries));
    if (!result.has_value()) {
        throw std::runtime_error("Failed to construct benchmark config");
    }
    return std::move(result.value());
}

RuntimeOptions make_options(
    int streams, int records_per_stream, int producers, std::size_t payload_bytes) {
    RuntimeOptions options;
    options.target_uri = "file://benchmark";
    options.session_id = "m2-benchmark";
    options.payload_block_bytes = payload_bytes;
    options.payload_block_count =
        static_cast<std::size_t>(streams * records_per_stream * producers + 1);
    options.max_single_payload_bytes = payload_bytes;
    options.dispatch_capacity = options.payload_block_count;
    options.operation_capacity = options.dispatch_capacity;
    options.completion_capacity = options.dispatch_capacity;
    options.submit_wait_timeout = std::chrono::seconds(5);
    options.stop_progress_timeout = std::chrono::seconds(5);
    options.storage.path = required_path("RODAQ_FAKE_STORAGE_PLUGIN");
    options.storage.config = config({
        {"scenario", plugin::ConfigTree::String("sync")},
        {"event_log", plugin::ConfigTree::String("")},
        {"callback_delay_us", plugin::ConfigTree::Int64(0)},
        {"submit_delay_us", plugin::ConfigTree::Int64(0)},
        {"descriptor_scenario", plugin::ConfigTree::String("ready")},
    });
    RuntimePluginOptions acquisition;
    acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
    acquisition.config = config({
        {"streams", plugin::ConfigTree::Int64(streams)},
        {"records_per_stream", plugin::ConfigTree::Int64(records_per_stream)},
        {"producer_threads", plugin::ConfigTree::Int64(producers)},
        {"payload_size", plugin::ConfigTree::Int64(static_cast<std::int64_t>(payload_bytes))},
        {"payload_mode", plugin::ConfigTree::String("copy")},
        {"descriptor_change_after", plugin::ConfigTree::Int64(-1)},
        {"binding_scenario", plugin::ConfigTree::String("")},
        {"lifecycle_scenario", plugin::ConfigTree::String("")},
        {"pause_before_switch", plugin::ConfigTree::Int64(0)},
        {"stale_handle_file", plugin::ConfigTree::String("")},
        {"event_log", plugin::ConfigTree::String("")},
    });
    options.acquisitions.push_back(std::move(acquisition));
    return options;
}

void run_pipeline(
    benchmark::State& state, int streams, int records_per_stream, int producers,
    std::size_t payload_bytes) {
    const std::uint64_t records =
        static_cast<std::uint64_t>(streams * records_per_stream * producers);
    for (auto iteration : state) {
        static_cast<void>(iteration);
        Runtime runtime(make_options(streams, records_per_stream, producers, payload_bytes));
        auto configured = runtime.Configure();
        if (!configured.has_value()) {
            state.SkipWithError(std::string(configured.error().message()).c_str());
            return;
        }
        auto started = runtime.Start();
        if (!started.has_value()) {
            state.SkipWithError(std::string(started.error().message()).c_str());
            return;
        }

        const auto deadline = Clock::now() + std::chrono::seconds(10);
        bool completed = false;
        while (Clock::now() < deadline) {
            const auto stats = runtime.Stats();
            if (stats.has_value() && stats->accepted_records == records &&
                stats->inflight_records == 0) {
                completed = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!completed) {
            state.SkipWithError("Runtime pipeline did not converge before the benchmark deadline");
            return;
        }
        auto stopped = runtime.Stop();
        if (!stopped.has_value()) {
            state.SkipWithError(std::string(stopped.error().message()).c_str());
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(records));
    state.SetBytesProcessed(
        state.iterations() * static_cast<std::int64_t>(records * payload_bytes));
}

void BM_RuntimePipelineSingleProducer(benchmark::State& state) {
    run_pipeline(state, 1, 256, 1, 1024);
}

void BM_RuntimePipelineConcurrent(benchmark::State& state) { run_pipeline(state, 4, 64, 4, 1024); }

}  // namespace
}  // namespace rodaq::runtime

BENCHMARK(rodaq::runtime::BM_RuntimePipelineSingleProducer)
    ->Iterations(20)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(rodaq::runtime::BM_RuntimePipelineConcurrent)
    ->Iterations(10)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
