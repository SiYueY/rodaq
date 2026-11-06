#ifndef RODAQ_BENCHMARKS_BENCHMARK_SUPPORT_HPP_
#define RODAQ_BENCHMARKS_BENCHMARK_SUPPORT_HPP_

#include <rodaq/config/config.hpp>
#include <rodaq/runtime/runtime.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/resource.h>

namespace rodaq::benchmark_support {

using Clock = std::chrono::steady_clock;

inline constexpr double kStallToleranceSeconds = 0.25;
inline constexpr std::size_t kSafetyFactor = 2;

inline std::size_t record_capacity(std::uint64_t peak_records_per_second) {
    return static_cast<std::size_t>(std::ceil(
        static_cast<double>(peak_records_per_second) * kStallToleranceSeconds *
        static_cast<double>(kSafetyFactor)));
}

inline std::filesystem::path required_path(const char* environment_name) {
    const char* value = std::getenv(environment_name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("Missing benchmark environment: ") + environment_name);
    }
    return std::filesystem::path(value);
}

inline ConfigTree config(std::vector<ConfigTree::MapEntry> entries) {
    auto result = ConfigTree::map(std::move(entries));
    if (!result.ok()) {
        throw std::runtime_error("Failed to construct benchmark config");
    }
    return std::move(result.value());
}

inline runtime::PluginConfig acquisition(
    std::size_t streams, std::size_t records_per_stream, std::size_t payload_bytes,
    std::uint64_t record_interval_ns, std::string payload_mode = "copy",
    std::string source_id = "benchmark-source") {
    runtime::PluginConfig value;
    value.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
    value.config = config({
        {"source_id", ConfigTree::string(std::move(source_id))},
        {"streams", ConfigTree::int64(static_cast<std::int64_t>(streams))},
        {"records_per_stream", ConfigTree::int64(static_cast<std::int64_t>(records_per_stream))},
        {"producer_threads", ConfigTree::int64(static_cast<std::int64_t>(streams))},
        {"producer_stream_mode", ConfigTree::string("exclusive")},
        {"record_interval_us",
         ConfigTree::int64(static_cast<std::int64_t>(record_interval_ns / 1'000U))},
        {"payload_size", ConfigTree::int64(static_cast<std::int64_t>(payload_bytes))},
        {"payload_mode", ConfigTree::string(std::move(payload_mode))},
        {"descriptor_change_after", ConfigTree::int64(-1)},
        {"binding_scenario", ConfigTree::string("")},
        {"lifecycle_scenario", ConfigTree::string("")},
        {"pause_before_switch", ConfigTree::int64(0)},
        {"stale_handle_file", ConfigTree::string("")},
        {"event_log", ConfigTree::string("")},
    });
    return value;
}

inline runtime::RuntimeConfig runtime_config(
    std::string session, std::vector<PayloadClassConfig> payload_classes,
    std::size_t operation_capacity, std::uint64_t maximum_payload_inflight_bytes,
    std::size_t maximum_streams, std::vector<runtime::PluginConfig> acquisitions) {
    runtime::RuntimeConfig value;
    value.session_id = std::move(session);
    value.target_uri = "file://capacity-benchmark";
    value.resources.payload_pool.classes = std::move(payload_classes);
    value.resources.max_single_payload_bytes =
        value.resources.payload_pool.classes.back().max_payload_bytes;
    value.resources.dispatch_capacity = operation_capacity;
    value.resources.operation_capacity = operation_capacity;
    value.resources.completion_capacity = operation_capacity;
    value.resources.payload_release_workers = 1;
    value.resources.max_streams = std::max<std::size_t>(64, maximum_streams);
    value.resources.max_stream_descriptors = value.resources.max_streams;
    value.resources.max_bindings = value.resources.max_streams;
    value.submit_wait_timeout = std::chrono::seconds(30);
    value.stop_progress_timeout = std::chrono::seconds(30);
    value.logging.level = RODAQ_LOG_WARN;
    value.logging.stderr_sink = false;
    value.storage.path = required_path("RODAQ_FAKE_STORAGE_PLUGIN");
    value.storage.config = config({
        {"scenario", ConfigTree::string("sync")},
        {"event_log", ConfigTree::string("")},
        {"callback_delay_us", ConfigTree::int64(0)},
        {"submit_delay_us", ConfigTree::int64(0)},
        {"descriptor_scenario", ConfigTree::string("ready")},
        {"maximum_inflight_records",
         ConfigTree::int64(static_cast<std::int64_t>(operation_capacity))},
        {"maximum_inflight_bytes",
         ConfigTree::int64(static_cast<std::int64_t>(maximum_payload_inflight_bytes))},
    });
    value.acquisitions = std::move(acquisitions);
    return value;
}

struct RunResult {
    bool passed = false;
    std::string error;
    RuntimeStatsSnapshot stats;
    double elapsed_seconds = 0.0;
    double cpu_seconds = 0.0;
    std::int64_t maximum_rss_kb = 0;
};

inline double process_cpu_seconds() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1'000'000.0;
}

inline std::int64_t maximum_rss_kb() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(usage.ru_maxrss);
}

inline RunResult execute(
    runtime::RuntimeConfig config, std::uint64_t expected_records,
    std::chrono::steady_clock::duration deadline_duration) {
    RunResult result;
    double cpu_started = 0.0;
    Clock::time_point started{};
    try {
        runtime::Runtime runtime(std::move(config));
        auto configured = runtime.configure();
        if (!configured.ok()) {
            result.error = std::string(configured.status().message());
            return result;
        }
        auto started_result = runtime.start();
        if (!started_result.ok()) {
            result.error = std::string(started_result.status().message());
            return result;
        }
        cpu_started = process_cpu_seconds();
        started = Clock::now();

        const auto deadline = Clock::now() + deadline_duration;
        bool converged = false;
        while (Clock::now() < deadline) {
            auto stats = runtime.stats();
            if (stats.ok()) {
                if (stats->accepted_records == expected_records && stats->inflight_records == 0) {
                    converged = true;
                    break;
                }
            }
            if (runtime.state() == runtime::RuntimeState::Failed) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if (!converged) {
            result.error = "Runtime workload did not converge before the deadline";
        }
        const auto measurement_ended = Clock::now();
        const double cpu_at_end = process_cpu_seconds();
        auto stopped = runtime.stop();
        auto stats = runtime.stats();
        if (stats.ok()) {
            result.stats = std::move(stats.value());
            result.passed =
                converged && stopped.ok() && result.stats.accepted_records == expected_records &&
                result.stats.inflight_records == 0 && result.stats.timed_out_records == 0 &&
                result.stats.rejected_records == 0 && result.stats.gate_closed_records == 0;
        } else if (stopped.ok() && converged) {
            result.error = "Runtime statistics are unavailable";
        } else if (!stopped.ok() && result.error.empty()) {
            result.error = std::string(stopped.status().message());
        }
        if (started != Clock::time_point{}) {
            result.elapsed_seconds =
                std::chrono::duration<double>(measurement_ended - started).count();
            result.cpu_seconds = cpu_at_end - cpu_started;
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    if (started != Clock::time_point{} && result.elapsed_seconds == 0.0) {
        result.elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
        result.cpu_seconds = process_cpu_seconds() - cpu_started;
    }
    result.maximum_rss_kb = maximum_rss_kb();
    return result;
}

inline void report(
    benchmark::State& state, const RunResult& result, std::uint64_t expected_records,
    std::uint64_t expected_bytes) {
    if (!result.passed) {
        state.SkipWithError(
            result.error.empty() ? "Runtime capacity profile failed" : result.error.c_str());
        return;
    }
    const double elapsed = std::max(result.elapsed_seconds, 1e-9);
    state.counters["offered_records_per_second"] = static_cast<double>(expected_records) / elapsed;
    state.counters["accepted_records_per_second"] =
        static_cast<double>(result.stats.accepted_records) / elapsed;
    state.counters["accepted_bytes_per_second"] =
        static_cast<double>(result.stats.accepted_bytes) / elapsed;
    state.counters["peak_cpu_fraction"] = result.cpu_seconds / elapsed;
    state.counters["maximum_rss_kb"] = static_cast<double>(result.maximum_rss_kb);
    state.counters["high_water_records"] = static_cast<double>(result.stats.high_water_records);
    state.counters["high_water_bytes"] = static_cast<double>(result.stats.high_water_bytes);
    const auto report_latency = [&state](const char* prefix, const LatencySnapshot& latency) {
        state.counters[std::string(prefix) + "_p50_us"] =
            static_cast<double>(latency.p50_ns) / 1000.0;
        state.counters[std::string(prefix) + "_p95_us"] =
            static_cast<double>(latency.p95_ns) / 1000.0;
        state.counters[std::string(prefix) + "_p99_us"] =
            static_cast<double>(latency.p99_ns) / 1000.0;
        state.counters[std::string(prefix) + "_p99_9_us"] =
            static_cast<double>(latency.p999_ns) / 1000.0;
    };
    report_latency("admission", result.stats.latencies.admission_wait);
    report_latency("handoff", result.stats.latencies.storage_handoff);
    report_latency("payload", result.stats.latencies.payload_consumed);
    report_latency("persistence", result.stats.latencies.persistence);
    state.counters["stop_us"] = static_cast<double>(result.stats.latencies.stop.p50_ns) / 1000.0;
    state.SetItemsProcessed(
        state.iterations() * static_cast<std::int64_t>(result.stats.accepted_records));
    state.SetBytesProcessed(
        state.iterations() * static_cast<std::int64_t>(result.stats.accepted_bytes));
    static_cast<void>(expected_bytes);
}

inline void run_profile(
    benchmark::State& state, const runtime::RuntimeConfig& config, std::uint64_t expected_records,
    std::uint64_t expected_bytes,
    std::chrono::steady_clock::duration deadline = std::chrono::seconds(15),
    double maximum_schedule_seconds = 0.0) {
    RunResult last;
    for (auto iteration : state) {
        static_cast<void>(iteration);
        last = execute(config, expected_records, deadline);
        if (last.passed && maximum_schedule_seconds > 0.0 &&
            last.elapsed_seconds > maximum_schedule_seconds) {
            last.passed = false;
            last.error = "Runtime workload missed the offered schedule deadline";
        }
        if (!last.passed) {
            break;
        }
    }
    report(state, last, expected_records, expected_bytes);
}

}  // namespace rodaq::benchmark_support

#endif  // RODAQ_BENCHMARKS_BENCHMARK_SUPPORT_HPP_
