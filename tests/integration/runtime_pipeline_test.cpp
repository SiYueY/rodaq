#include "plugin/config_view.hpp"
#include "runtime/quarantine.hpp"
#include "runtime/runtime.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

namespace rodaq::runtime {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

fs::path required_path(const char* environment_name) {
    const char* value = std::getenv(environment_name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("Missing test environment: ") + environment_name);
    }
    return fs::path(value);
}

std::string unique_log_path(std::string_view name) {
    return (fs::temp_directory_path() /
            ("rodaq-" + std::string(name) + "-" + std::to_string(::getpid()) + ".log"))
        .string();
}

plugin::ConfigTree map_config(std::vector<plugin::ConfigTree::MapEntry> entries) {
    auto result = plugin::ConfigTree::map(std::move(entries));
    if (!result.has_value()) {
        throw std::runtime_error("Failed to construct test config");
    }
    return std::move(result.value());
}

RuntimeConfig options(
    std::string_view name, std::string storage_scenario, std::string payload_mode, int streams,
    int records, int producers, std::string event_log,
    std::chrono::milliseconds stop_timeout = std::chrono::milliseconds(500),
    int descriptor_change_after = -1, std::int64_t payload_size = 16,
    std::size_t block_bytes = 64U * 1024U, std::size_t capacity = 8,
    AdmissionMode admission_mode = AdmissionMode::Wait, int submit_delay_us = 0,
    std::string descriptor_scenario = "ready", std::string binding_scenario = "",
    std::string lifecycle_scenario = "", bool pause_before_switch = false,
    std::string stale_handle_file = "", int callback_delay_us = 1000,
    std::int64_t maximum_inflight_records = 128,
    std::int64_t maximum_inflight_bytes = 128LL * 1024LL * 1024LL,
    std::string required_projection_id = "", std::string projection_id = "",
    bool projection_before_start = false, int64_t projection_delay_after = -1,
    std::string projection_registration_mode = "", std::string timestamp_scenario = "",
    int timestamp_key_count = 1, std::size_t completion_capacity = 0,
    int64_t descriptor_stall_after = -1, bool plugin_local_lock = false,
    std::string producer_stream_mode = "all", int64_t record_interval_us = 0,
    int64_t plugin_health_after = -1, std::string plugin_health_scenario = "",
    std::string payload_release_block_file = "") {
    RuntimeConfig value;
    value.target_uri = "file://fake";
    value.session_id = std::string(name);
    value.resources.payload_pool.classes = {
        PayloadClassConfig{block_bytes, std::max<std::size_t>(16, capacity)}};
    value.resources.max_single_payload_bytes = block_bytes;
    value.resources.dispatch_capacity = capacity;
    value.resources.operation_capacity = capacity;
    value.resources.completion_capacity = completion_capacity == 0 ? capacity : completion_capacity;
    value.submit_wait_timeout = std::chrono::milliseconds(500);
    value.stop_progress_timeout = stop_timeout;
    value.admission_mode = admission_mode;
    value.storage.path = required_path("RODAQ_FAKE_STORAGE_PLUGIN");
    value.storage.config = map_config({
        {"scenario", plugin::ConfigTree::string(std::move(storage_scenario))},
        {"event_log", plugin::ConfigTree::string(event_log)},
        {"callback_delay_us", plugin::ConfigTree::int64(callback_delay_us)},
        {"submit_delay_us", plugin::ConfigTree::int64(submit_delay_us)},
        {"descriptor_scenario", plugin::ConfigTree::string(std::move(descriptor_scenario))},
        {"descriptor_stall_after", plugin::ConfigTree::int64(descriptor_stall_after)},
        {"maximum_inflight_records", plugin::ConfigTree::int64(maximum_inflight_records)},
        {"maximum_inflight_bytes", plugin::ConfigTree::int64(maximum_inflight_bytes)},
        {"required_projection_id", plugin::ConfigTree::string(std::move(required_projection_id))},
    });
    PluginConfig acquisition;
    acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
    acquisition.config = map_config({
        {"streams", plugin::ConfigTree::int64(streams)},
        {"records_per_stream", plugin::ConfigTree::int64(records)},
        {"producer_threads", plugin::ConfigTree::int64(producers)},
        {"payload_size", plugin::ConfigTree::int64(payload_size)},
        {"payload_mode", plugin::ConfigTree::string(std::move(payload_mode))},
        {"producer_stream_mode", plugin::ConfigTree::string(std::move(producer_stream_mode))},
        {"record_interval_us", plugin::ConfigTree::int64(record_interval_us)},
        {"descriptor_change_after", plugin::ConfigTree::int64(descriptor_change_after)},
        {"binding_scenario", plugin::ConfigTree::string(std::move(binding_scenario))},
        {"lifecycle_scenario", plugin::ConfigTree::string(std::move(lifecycle_scenario))},
        {"pause_before_switch", plugin::ConfigTree::int64(pause_before_switch ? 1 : 0)},
        {"stale_handle_file", plugin::ConfigTree::string(std::move(stale_handle_file))},
        {"projection_id", plugin::ConfigTree::string(std::move(projection_id))},
        {"projection_before_start", plugin::ConfigTree::int64(projection_before_start ? 1 : 0)},
        {"projection_delay_after", plugin::ConfigTree::int64(projection_delay_after)},
        {"projection_registration_mode",
         plugin::ConfigTree::string(std::move(projection_registration_mode))},
        {"plugin_local_lock", plugin::ConfigTree::int64(plugin_local_lock ? 1 : 0)},
        {"timestamp_scenario", plugin::ConfigTree::string(std::move(timestamp_scenario))},
        {"timestamp_key_count", plugin::ConfigTree::int64(timestamp_key_count)},
        {"plugin_health_after", plugin::ConfigTree::int64(plugin_health_after)},
        {"plugin_health_scenario", plugin::ConfigTree::string(std::move(plugin_health_scenario))},
        {"payload_release_block_file",
         plugin::ConfigTree::string(std::move(payload_release_block_file))},
        {"event_log", plugin::ConfigTree::string(event_log)},
    });
    value.acquisitions.push_back(std::move(acquisition));
    return value;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

bool wait_for_records(
    Runtime& runtime, const std::string& log_path, std::size_t expected,
    std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        std::size_t count = 0;
        for (const std::string& line : read_lines(log_path)) {
            if (line.rfind("record ", 0) == 0) {
                ++count;
            }
        }
        if (count >= expected) {
            return true;
        }
        if (runtime.state() == RuntimeState::Failed) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool wait_for_log_prefix(
    Runtime& runtime, const std::string& path, std::string_view prefix, std::size_t expected,
    std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        const auto lines = read_lines(path);
        const auto count = std::count_if(lines.begin(), lines.end(), [prefix](const auto& line) {
            return line.rfind(prefix, 0) == 0;
        });
        if (static_cast<std::size_t>(count) >= expected) {
            return true;
        }
        if (runtime.state() == RuntimeState::Failed) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

bool wait_for_file(
    const std::string& path, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (fs::is_regular_file(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::size_t record_count(const std::string& path) {
    const auto lines = read_lines(path);
    return static_cast<std::size_t>(std::count_if(
        lines.begin(), lines.end(),
        [](const std::string& line) { return line.rfind("record ", 0) == 0; }));
}

bool log_contains(const std::string& path, std::string_view expected) {
    const auto lines = read_lines(path);
    return std::any_of(lines.begin(), lines.end(), [expected](const auto& line) {
        return line.find(expected) != std::string::npos;
    });
}

std::size_t log_count(const std::string& path, std::string_view expected) {
    const auto lines = read_lines(path);
    return static_cast<std::size_t>(std::count_if(
        lines.begin(), lines.end(),
        [expected](auto& line) { return line.find(expected) != std::string::npos; }));
}

void expect_clean_scenario(
    std::string_view name, std::string storage_scenario, std::string payload_mode,
    int records = 4) {
    const std::string log = unique_log_path(std::string(name));
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(
        options(name, std::move(storage_scenario), std::move(payload_mode), 1, records, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, static_cast<std::size_t>(records)));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Stopped);
    EXPECT_EQ(record_count(log), static_cast<std::size_t>(records));
}

void expect_failed_scenario(
    std::string_view name, std::string storage_scenario, std::chrono::milliseconds stop_timeout) {
    const std::string log = unique_log_path(std::string(name));
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(name, std::move(storage_scenario), "copy", 1, 1, 1, log, stop_timeout));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.primary_failure().has_value());
}

bool wait_for_failure(Runtime& runtime) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (runtime.state() != RuntimeState::Failed && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return runtime.state() == RuntimeState::Failed;
}

TEST(RuntimePipelineTest, RejectsInvalidLifecycleTransitions) {
    const auto log = unique_log_path("lifecycle");
    Runtime runtime(options("lifecycle", "sync", "copy", 1, 0, 1, log));
    EXPECT_EQ(runtime.state(), RuntimeState::Constructed);
    EXPECT_FALSE(runtime.start().has_value());
    ASSERT_TRUE(runtime.configure().has_value());
    EXPECT_FALSE(runtime.configure().has_value());
}

TEST(RuntimePipelineTest, CopiesRecordsThroughStableAbiAndStopsCleanly) {
    const auto log = unique_log_path("sync");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("sync", "sync", "copy", 1, 3, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_EQ(runtime.state(), RuntimeState::Running);
    const auto running_snapshot = runtime.stats();
    ASSERT_TRUE(running_snapshot.has_value());
    EXPECT_GE(running_snapshot->logging.records_submitted, 1U);
    ASSERT_TRUE(wait_for_records(runtime, log, 3));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Stopped);

    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 3U);
    EXPECT_EQ(snapshot->accepted_bytes, 48U);
    EXPECT_EQ(snapshot->inflight_records, 0U);
    EXPECT_EQ(snapshot->inflight_bytes, 0U);
    EXPECT_GE(snapshot->high_water_records, 1U);
    EXPECT_LE(snapshot->high_water_records, 3U);
    EXPECT_GE(snapshot->high_water_bytes, 16U);
    EXPECT_LE(snapshot->high_water_bytes, 48U);
    EXPECT_EQ(snapshot->latencies.admission_wait.count, 3U);
    EXPECT_EQ(snapshot->latencies.storage_handoff.count, 3U);
    EXPECT_EQ(snapshot->latencies.payload_consumed.count, 3U);
    EXPECT_EQ(snapshot->latencies.persistence.count, 3U);
    EXPECT_EQ(snapshot->latencies.stop.count, 1U);
    EXPECT_GE(snapshot->latencies.persistence.p99_ns, snapshot->latencies.persistence.p50_ns);
    const auto payload_stats = runtime.payload_stats();
    ASSERT_TRUE(payload_stats.has_value());
    for (const auto& payload_class : payload_stats->classes) {
        EXPECT_EQ(payload_class.used_blocks, 0U);
        EXPECT_EQ(payload_class.current_physical_bytes, 0U);
    }
    std::vector<std::string> records;
    for (const std::string& line : read_lines(log)) {
        if (line.rfind("record ", 0) == 0) {
            records.push_back(line);
        }
    }
    ASSERT_EQ(records.size(), 3U);
    EXPECT_NE(records[0].find("sequence=0"), std::string::npos);
    EXPECT_NE(records[1].find("sequence=1"), std::string::npos);
    EXPECT_NE(records[2].find("sequence=2"), std::string::npos);
}

TEST(RuntimePipelineTest, RecoverablePluginFailureBlocksNewSubmissionsUntilRebinding) {
    const std::string log = unique_log_path("plugin_health_recoverable");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "plugin_health_recoverable", "sync", "copy", 1, 4, 1, log, std::chrono::milliseconds(500),
        -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 128,
        128LL * 1024 * 1024, "", "", false, -1, "", "", 1, 0, -1, false, "all", 0, 0,
        "recoverable"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "control plugin_health", 1));
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 4));
    EXPECT_EQ(RuntimeState::Running, runtime.state());
    ASSERT_TRUE(runtime.stop().has_value());
    const auto stats = runtime.stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(1U, stats->accepted_records);
    EXPECT_GE(stats->rejected_records, 1U);
}

TEST(RuntimePipelineTest, AvailablePluginHealthRequiresNewDescriptorAndBinding) {
    const std::string log = unique_log_path("plugin_health_recovered");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "plugin_health_recovered", "sync", "copy", 1, 4, 1, log, std::chrono::milliseconds(500), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 128,
        128LL * 1024 * 1024, "", "", false, -1, "", "", 1, 0, -1, false, "all", 0, 0, "recovered"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 4));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(log_contains(log, "control plugin_health state=recovered"));
    const auto stats = runtime.stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(4U, stats->accepted_records);
}

TEST(RuntimePipelineTest, TerminalPluginFailureStopsTheActivation) {
    const std::string log = unique_log_path("plugin_health_terminal");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "plugin_health_terminal", "sync", "copy", 1, 2, 1, log, std::chrono::milliseconds(500), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 128,
        128LL * 1024 * 1024, "", "", false, -1, "", "", 1, 0, -1, false, "all", 0, 0, "terminal"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_failure(runtime));
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_TRUE(runtime.primary_failure().has_value());
}

TEST(RuntimePipelineTest, RetainsPayloadThroughAsynchronousDualCompletion) {
    const auto log = unique_log_path("retain_async");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(
        options("retain_async", "async_payload_async_persistence", "retain", 1, 4, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 4));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Stopped);
    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 4U);
    EXPECT_EQ(snapshot->accepted_bytes, 64U);
    EXPECT_EQ(snapshot->inflight_records, 0U);
    EXPECT_EQ(snapshot->inflight_bytes, 0U);
}

TEST(RuntimePipelineTest, QualifiesIndependentCompletionAxes) {
    expect_clean_scenario(
        "sync_payload_async_persistence", "sync_payload_async_persistence", "copy");
    expect_clean_scenario(
        "async_payload_sync_persistence", "async_payload_sync_persistence", "adopt");
    expect_clean_scenario(
        "async_payload_async_persistence", "async_payload_async_persistence", "retain");
    expect_clean_scenario("persistence_first", "persistence_first", "copy");
    expect_clean_scenario("persistence_failed", "persistence_failed", "retain");
    expect_clean_scenario("persistence_unknown", "persistence_unknown", "adopt");
}

TEST(RuntimePipelineTest, ReleasesInflightPayloadBytesWhilePersistenceIsPending) {
    const std::string log = unique_log_path("payload_early_release");
    std::error_code removed;
    fs::remove(log, removed);
    constexpr std::size_t kPayloadBytes = 1024U * 1024U;
    Runtime runtime(options(
        "payload_early_release", "sync_payload_async_persistence", "copy", 1, 1, 1, log,
        std::chrono::seconds(1), -1, static_cast<std::int64_t>(kPayloadBytes), kPayloadBytes, 8,
        AdmissionMode::Wait, 0, "ready", "", "", false, "", 300'000));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));

    bool observed_early_release = false;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        const auto snapshot = runtime.stats();
        if (snapshot.has_value() && snapshot->accepted_records == 1U &&
            snapshot->inflight_records == 1U && snapshot->inflight_bytes == 0U) {
            observed_early_release = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(observed_early_release);
    ASSERT_TRUE(runtime.stop().has_value());
    const auto final_snapshot = runtime.stats();
    ASSERT_TRUE(final_snapshot.has_value());
    EXPECT_EQ(final_snapshot->inflight_records, 0U);
    EXPECT_EQ(final_snapshot->inflight_bytes, 0U);
}

TEST(RuntimePipelineTest, PreservesPerStreamSequenceOrderAcrossProducers) {
    const auto log = unique_log_path("multi_stream");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("multi_stream", "sync", "adopt", 2, 5, 2, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 20));
    ASSERT_TRUE(runtime.stop().has_value());

    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> sequences;
    for (const std::string& line : read_lines(log)) {
        if (line.rfind("record ", 0) != 0) {
            continue;
        }
        std::istringstream input(line);
        std::string token;
        std::uint64_t stream = 0;
        std::uint64_t sequence = 0;
        while (input >> token) {
            if (token.rfind("stream=", 0) == 0) {
                stream = std::stoull(token.substr(7));
            } else if (token.rfind("sequence=", 0) == 0) {
                sequence = std::stoull(token.substr(9));
            }
        }
        auto& values = sequences[stream];
        if (!values.empty()) {
            EXPECT_GT(sequence, values.back());
        }
        values.push_back(sequence);
    }
    ASSERT_EQ(sequences.size(), 2U);
    for (const auto& entry : sequences) {
        EXPECT_EQ(entry.second.size(), 10U);
    }
}

TEST(RuntimePipelineTest, ProducesJointRateControlledExclusiveStreams) {
    constexpr int kStreams = 2;
    constexpr int kRecordsPerStream = 5;
    constexpr int kIntervalUs = 2'000;
    const std::string log = unique_log_path("rate_controlled_joint_streams");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "rate_controlled_joint_streams", "sync", "copy", kStreams, kRecordsPerStream, kStreams, log,
        std::chrono::seconds(1), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "",
        false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 1, 0, -1, false,
        "exclusive", kIntervalUs));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    const auto started = Clock::now();
    ASSERT_TRUE(wait_for_records(runtime, log, kStreams * kRecordsPerStream));
    EXPECT_GE(Clock::now() - started, std::chrono::milliseconds(5));
    ASSERT_TRUE(runtime.stop().has_value());
    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, kStreams * kRecordsPerStream);
    EXPECT_EQ(snapshot->rejected_records, 0U);
    EXPECT_EQ(record_count(log), static_cast<std::size_t>(kStreams * kRecordsPerStream));
}

TEST(RuntimePipelineTest, SwitchesDescriptorWithoutBreakingStreamOrdering) {
    const auto log = unique_log_path("descriptor_switch");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "descriptor_switch", "sync", "copy", 1, 6, 1, log, std::chrono::milliseconds(500), 2));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 6));
    ASSERT_TRUE(runtime.stop().has_value());

    std::vector<std::uint64_t> descriptors;
    std::vector<std::uint64_t> sequences;
    for (const std::string& line : read_lines(log)) {
        if (line.rfind("record ", 0) != 0) {
            continue;
        }
        std::istringstream input(line);
        std::string token;
        while (input >> token) {
            if (token.rfind("descriptor=", 0) == 0) {
                descriptors.push_back(std::stoull(token.substr(11)));
            } else if (token.rfind("sequence=", 0) == 0) {
                sequences.push_back(std::stoull(token.substr(9)));
            }
        }
    }
    ASSERT_EQ(descriptors.size(), 6U);
    ASSERT_EQ(sequences.size(), 6U);
    EXPECT_EQ(
        static_cast<std::size_t>(std::count_if(
            descriptors.begin(), descriptors.end(),
            [&descriptors](std::uint64_t value) { return value == descriptors.front(); })),
        2U);
    for (std::size_t index = 1; index < sequences.size(); ++index) {
        EXPECT_GT(sequences[index], sequences[index - 1U]);
    }
}

TEST(RuntimePipelineTest, RejectsDescriptorReadinessAndInitialBindingFailures) {
    const std::string failed_log = unique_log_path("readiness_failed");
    std::error_code failed_removed;
    fs::remove(failed_log, failed_removed);
    Runtime failed(options(
        "readiness_failed", "sync", "copy", 1, 1, 1, failed_log, std::chrono::milliseconds(100), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "failed"));
    ASSERT_TRUE(failed.configure().has_value());
    EXPECT_FALSE(failed.start().has_value());
    EXPECT_EQ(failed.state(), RuntimeState::Failed);
    auto failed_primary = failed.primary_failure();
    ASSERT_TRUE(failed_primary.has_value());
    EXPECT_EQ(failed_primary->code(), ErrorCode::StorageError);
    auto failed_secondary = failed.secondary_failures();
    ASSERT_TRUE(failed_secondary.has_value());
    EXPECT_TRUE(failed_secondary->empty());
    EXPECT_EQ(failed.secondary_failure_overflow_count(), 0U);

    const std::string unsupported_log = unique_log_path("readiness_unsupported");
    std::error_code unsupported_removed;
    fs::remove(unsupported_log, unsupported_removed);
    Runtime unsupported(options(
        "readiness_unsupported", "sync", "copy", 1, 1, 1, unsupported_log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0,
        "unsupported"));
    ASSERT_TRUE(unsupported.configure().has_value());
    ASSERT_TRUE(unsupported.start().has_value());
    EXPECT_EQ(unsupported.state(), RuntimeState::Running);
    ASSERT_TRUE(unsupported.stop().has_value());
    EXPECT_EQ(record_count(unsupported_log), 0U);

    const std::string log = unique_log_path("initial_prepare_failure");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "initial_prepare_failure", "sync", "copy", 1, 1, 1, log, std::chrono::milliseconds(100), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "prepare_failure_initial"));
    ASSERT_TRUE(runtime.configure().has_value());
    EXPECT_FALSE(runtime.start().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Unavailable);
    auto secondary = runtime.secondary_failures();
    ASSERT_TRUE(secondary.has_value());
    EXPECT_TRUE(secondary->empty());
    EXPECT_EQ(runtime.secondary_failure_overflow_count(), 0U);
    EXPECT_TRUE(log_contains(log, "control abort"));
    EXPECT_TRUE(log_contains(log, "disposition=1"));
}

TEST(RuntimePipelineTest, ReturnsDescriptorHandleBeforeInitialReadinessCompletes) {
    const std::string log = unique_log_path("descriptor_publish_order");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "descriptor_publish_order", "sync", "copy", 1, 1, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "publish_order"));
    ASSERT_TRUE(runtime.configure().has_value());
    const auto started = Clock::now();
    ASSERT_TRUE(runtime.start().has_value());
    const auto elapsed = Clock::now() - started;
    EXPECT_GE(elapsed, std::chrono::milliseconds(50));
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());

    const auto lines = read_lines(log);
    const auto published = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.rfind("control descriptor_published descriptor=", 0) == 0;
    });
    const auto ready = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.rfind("control storage_readiness descriptor=", 0) == 0;
    });
    ASSERT_NE(published, lines.end());
    ASSERT_NE(ready, lines.end());
    EXPECT_LT(published, ready);
}

TEST(RuntimePipelineTest, BoundsDescriptorReconciliationRequestQueue) {
    const std::string log = unique_log_path("descriptor_worker_capacity");
    std::error_code removed;
    fs::remove(log, removed);
    auto runtime_options = options(
        "descriptor_worker_capacity", "sync", "copy", 3, 1, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "publish_order");
    runtime_options.resources.max_stream_descriptors = 1;
    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.configure().has_value());
    const auto started = runtime.start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::ResourceExhausted);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::ResourceExhausted);
}

TEST(RuntimePipelineTest, PreservesStorageOpenAndAcquisitionStartFailures) {
    const std::string storage_log = unique_log_path("storage_open_failure");
    std::error_code storage_removed;
    fs::remove(storage_log, storage_removed);
    Runtime storage(options(
        "storage_open_failure", "open_failure", "copy", 1, 1, 1, storage_log,
        std::chrono::milliseconds(100)));
    ASSERT_TRUE(storage.configure().has_value());
    EXPECT_FALSE(storage.start().has_value());
    EXPECT_EQ(storage.state(), RuntimeState::Failed);
    auto storage_primary = storage.primary_failure();
    ASSERT_TRUE(storage_primary.has_value());
    EXPECT_EQ(storage_primary->code(), ErrorCode::StorageError);
    EXPECT_NE(storage_primary->message().find("open failure"), std::string_view::npos);

    const std::string acquisition_log = unique_log_path("acquisition_start_failure");
    std::error_code acquisition_removed;
    fs::remove(acquisition_log, acquisition_removed);
    Runtime acquisition(options(
        "acquisition_start_failure", "sync", "copy", 1, 1, 1, acquisition_log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "",
        "start_failure"));
    ASSERT_TRUE(acquisition.configure().has_value());
    EXPECT_FALSE(acquisition.start().has_value());
    EXPECT_EQ(acquisition.state(), RuntimeState::Failed);
    auto acquisition_primary = acquisition.primary_failure();
    ASSERT_TRUE(acquisition_primary.has_value());
    EXPECT_EQ(acquisition_primary->code(), ErrorCode::Unavailable);
    EXPECT_NE(
        acquisition_primary->message().find("configured acquisition start failure"),
        std::string_view::npos);
}

TEST(RuntimePipelineTest, ReconcilesProjectionDependentDescriptorReadiness) {
    constexpr std::string_view kProjection = "rodaq.projection.test.v1";
    const std::string log = unique_log_path("projection_readiness");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "projection_readiness", "sync", "copy", 1, 1, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), true));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(log_contains(log, "storage_readiness descriptor="));
    EXPECT_TRUE(log_contains(log, "storage_readiness projection=rodaq.projection.test.v1"));
    EXPECT_EQ(record_count(log), 1U);
}

TEST(RuntimePipelineTest, KeepsDescriptorPendingUntilProjectionRegistration) {
    constexpr std::string_view kProjection = "rodaq.projection.test.v1";
    const std::string log = unique_log_path("projection_pending");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "projection_pending", "sync", "copy", 1, 2, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), false, 1));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 2));
    ASSERT_TRUE(runtime.stop().has_value());
    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 1U);
    EXPECT_EQ(snapshot->rejected_records, 1U);
    EXPECT_EQ(record_count(log), 1U);
    EXPECT_TRUE(log_contains(log, "storage_readiness projection=rodaq.projection.test.v1"));
}

TEST(RuntimePipelineTest, DefersProjectionReconciliationOutsidePluginLocalLock) {
    constexpr std::string_view kProjection = "rodaq.projection.test.v1";
    const std::string log = unique_log_path("projection_plugin_lock");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "projection_plugin_lock", "sync", "copy", 1, 3, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), false, 1, "",
        "", 1, 0, -1, true));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 3));
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    ASSERT_TRUE(runtime.stop().has_value());
    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 2U);
    EXPECT_EQ(snapshot->rejected_records, 1U);
    EXPECT_EQ(record_count(log), 2U);
    EXPECT_TRUE(log_contains(log, "storage_readiness projection=rodaq.projection.test.v1"));
}

TEST(RuntimePipelineTest, SwitchesProjectionDependentDescriptorAndRejectsConflict) {
    constexpr std::string_view kProjection = "rodaq.projection.test.v1";
    const std::string log = unique_log_path("projection_switch");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "projection_switch", "sync", "copy", 1, 6, 1, log, std::chrono::seconds(1), 2, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), true));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 6));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_EQ(record_count(log), 6U);

    const std::string duplicate_log = unique_log_path("projection_duplicate");
    std::error_code duplicate_removed;
    fs::remove(duplicate_log, duplicate_removed);
    Runtime duplicate(options(
        "projection_duplicate", "sync", "copy", 1, 1, 1, duplicate_log, std::chrono::seconds(1), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000,
        128, 128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), true, -1,
        "same"));
    ASSERT_TRUE(duplicate.configure().has_value());
    ASSERT_TRUE(duplicate.start().has_value());
    ASSERT_TRUE(wait_for_records(duplicate, duplicate_log, 1));
    ASSERT_TRUE(duplicate.stop().has_value());

    const std::string conflict_log = unique_log_path("projection_conflict");
    std::error_code conflict_removed;
    fs::remove(conflict_log, conflict_removed);
    Runtime conflict(options(
        "projection_conflict", "sync", "copy", 1, 1, 1, conflict_log, std::chrono::seconds(1), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000,
        128, 128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), true, -1,
        "conflict"));
    ASSERT_TRUE(conflict.configure().has_value());
    EXPECT_FALSE(conflict.start().has_value());
    auto primary = conflict.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Conflict);
}

TEST(RuntimePipelineTest, InjectsMandatoryRuntimeIngressTimestamp) {
    const std::string log = unique_log_path("ingress_timestamp");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("ingress_timestamp", "sync", "copy", 1, 1, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(log_contains(log, "timestamp_count=2 ingress_present=1 sample_present=1"));
}

TEST(RuntimePipelineTest, CapturesIngressBeforeAdmissionWaitAndPayloadCopy) {
    constexpr std::size_t kLargePayloadBytes = 2U * 1024U * 1024U;
    const std::string log = unique_log_path("ingress_before_wait");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "ingress_before_wait", "sync", "copy", 1, 2, 1, log, std::chrono::seconds(2), -1,
        static_cast<std::int64_t>(kLargePayloadBytes), kLargePayloadBytes, 1, AdmissionMode::Wait,
        300'000, "ready", "", "", false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1,
        "", "monotonic", 1));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 2));
    ASSERT_TRUE(runtime.stop().has_value());

    std::size_t parsed = 0;
    for (const std::string& line : read_lines(log)) {
        if (line.rfind("record ", 0) != 0) {
            continue;
        }
        std::istringstream input(line);
        std::string token;
        std::int64_t sample_reading = 0;
        std::int64_t ingress_reading = 0;
        while (input >> token) {
            if (token.rfind("sample_reading=", 0) == 0) {
                sample_reading = std::stoll(token.substr(15));
            } else if (token.rfind("ingress_reading=", 0) == 0) {
                ingress_reading = std::stoll(token.substr(16));
            }
        }
        ASSERT_GT(sample_reading, 0);
        ASSERT_GT(ingress_reading, 0);
        EXPECT_GE(ingress_reading, sample_reading);
        EXPECT_LT(ingress_reading - sample_reading, 100'000'000);
        ++parsed;
    }
    EXPECT_EQ(parsed, 2U);
}

TEST(RuntimePipelineTest, EnforcesCanonicalTimestampCapacityAndRejectsIngressRole) {
    const std::string three_log = unique_log_path("three_transport_timestamps");
    std::error_code three_removed;
    fs::remove(three_log, three_removed);
    Runtime three(options(
        "three_transport_timestamps", "sync", "copy", 1, 1, 1, three_log, std::chrono::seconds(1),
        -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 3));
    ASSERT_TRUE(three.configure().has_value());
    ASSERT_TRUE(three.start().has_value());
    ASSERT_TRUE(wait_for_records(three, three_log, 1));
    ASSERT_TRUE(three.stop().has_value());
    EXPECT_TRUE(log_contains(three_log, "timestamp_count=4 ingress_present=1 sample_present=1"));

    const std::string four_log = unique_log_path("four_transport_timestamps");
    std::error_code four_removed;
    fs::remove(four_log, four_removed);
    Runtime four(options(
        "four_transport_timestamps", "sync", "copy", 1, 1, 1, four_log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "",
        "", false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 4));
    ASSERT_TRUE(four.configure().has_value());
    EXPECT_FALSE(four.start().has_value());
    auto four_failure = four.primary_failure();
    ASSERT_TRUE(four_failure.has_value());
    EXPECT_EQ(four_failure->code(), ErrorCode::InvalidArgument);

    const std::string ingress_log = unique_log_path("plugin_ingress_role");
    std::error_code ingress_removed;
    fs::remove(ingress_log, ingress_removed);
    Runtime ingress(options(
        "plugin_ingress_role", "sync", "copy", 1, 1, 1, ingress_log, std::chrono::milliseconds(100),
        -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, "", "", false, -1, "", "ingress_role", 1));
    ASSERT_TRUE(ingress.configure().has_value());
    EXPECT_FALSE(ingress.start().has_value());
    auto ingress_failure = ingress.primary_failure();
    ASSERT_TRUE(ingress_failure.has_value());
    EXPECT_EQ(ingress_failure->code(), ErrorCode::InvalidArgument);

    const std::string duplicate_log = unique_log_path("duplicate_timestamp_key");
    std::error_code duplicate_removed;
    fs::remove(duplicate_log, duplicate_removed);
    Runtime duplicate(options(
        "duplicate_timestamp_key", "sync", "copy", 1, 1, 1, duplicate_log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "",
        "", false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1, "", "duplicate", 2));
    ASSERT_TRUE(duplicate.configure().has_value());
    EXPECT_FALSE(duplicate.start().has_value());
    auto duplicate_failure = duplicate.primary_failure();
    ASSERT_TRUE(duplicate_failure.has_value());
    EXPECT_EQ(duplicate_failure->code(), ErrorCode::InvalidArgument);
    EXPECT_NE(duplicate_failure->message().find("duplicate key"), std::string_view::npos);
}

TEST(RuntimePipelineTest, ExposesStableActivationClockCorrelation) {
    const std::string log = unique_log_path("clock_correlation_probe");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "clock_correlation_probe", "sync", "copy", 1, 1, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "clock_probe"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(log_contains(log, "storage_clock_probe ingress="));
}

TEST(RuntimePipelineTest, RefreshesIngressClockAcrossRestart) {
    const std::string log = unique_log_path("clock_restart");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "clock_restart", "sync", "copy", 1, 1, 1, log, std::chrono::seconds(1), -1, 16, 64U * 1024U,
        8, AdmissionMode::Wait, 0, "clock_probe"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 2));
    ASSERT_TRUE(runtime.stop().has_value());

    std::vector<std::uint64_t> ingress_clocks;
    for (const std::string& line : read_lines(log)) {
        const std::size_t marker = line.find("storage_clock_probe ingress=");
        if (marker == std::string::npos) {
            continue;
        }
        const std::size_t begin = marker + std::string("storage_clock_probe ingress=").size();
        const std::size_t end = line.find(' ', begin);
        ingress_clocks.push_back(std::stoull(line.substr(begin, end - begin)));
    }
    ASSERT_EQ(ingress_clocks.size(), 2U);
    EXPECT_NE(ingress_clocks[0], ingress_clocks[1]);
}

TEST(RuntimePipelineTest, BoundsAndQuarantinesBlockedActivationCalls) {
    struct Scenario {
        const char* name;
        const char* storage_scenario;
        const char* descriptor_scenario;
        const char* lifecycle_scenario;
    };
    const Scenario scenarios[] = {
        {"storage_open_stall", "open_stall", "ready", ""},
        {"storage_capabilities_stall", "capabilities_stall", "ready", ""},
        {"descriptor_readiness_stall", "sync", "stall", ""},
        {"acquisition_start_stall", "sync", "ready", "start_stall"},
    };
    for (const Scenario& scenario : scenarios) {
        const std::string log = unique_log_path(scenario.name);
        std::error_code removed;
        fs::remove(log, removed);
        Runtime runtime(options(
            scenario.name, scenario.storage_scenario, "copy", 1, 1, 1, log,
            std::chrono::milliseconds(50), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0,
            scenario.descriptor_scenario, "", scenario.lifecycle_scenario));
        ASSERT_TRUE(runtime.configure().has_value());
        const auto started = Clock::now();
        EXPECT_FALSE(runtime.start().has_value());
        EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(500));
        EXPECT_EQ(runtime.state(), RuntimeState::Failed);
        auto primary = runtime.primary_failure();
        ASSERT_TRUE(primary.has_value());
        EXPECT_EQ(primary->code(), ErrorCode::Timeout);
    }
}

TEST(RuntimePipelineTest, AbortsBindingSwitchWithTheCorrectDisposition) {
    struct Scenario {
        const char* name;
        bool pause_before_switch;
        std::size_t expected_records;
        const char* expected_abort;
    };
    const Scenario scenarios[] = {
        {"resume_old", false, 6, "disposition=2"},
        {"keep_paused", true, 2, "disposition=1"},
    };
    for (const Scenario& scenario : scenarios) {
        const std::string log = unique_log_path(scenario.name);
        std::error_code removed;
        fs::remove(log, removed);
        Runtime runtime(options(
            scenario.name, "sync", "copy", 1, 6, 1, log, std::chrono::seconds(1), 2, 16,
            64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "prepare_failure_switch", "",
            scenario.pause_before_switch));
        ASSERT_TRUE(runtime.configure().has_value());
        ASSERT_TRUE(runtime.start().has_value());
        ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 5));
        ASSERT_TRUE(runtime.stop().has_value());
        EXPECT_EQ(runtime.state(), RuntimeState::Stopped);
        EXPECT_EQ(record_count(log), scenario.expected_records);
        EXPECT_TRUE(log_contains(log, scenario.expected_abort));
    }

    const std::string log = unique_log_path("commit_failure");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "commit_failure", "sync", "copy", 1, 6, 1, log, std::chrono::milliseconds(100), 2, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "commit_failure_switch"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_failure(runtime));
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_TRUE(log_contains(log, "control commit"));
}

TEST(RuntimePipelineTest, BoundsDescriptorReadinessDuringRunningSwitch) {
    const std::string log = unique_log_path("descriptor_readiness_running_stall");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "descriptor_readiness_running_stall", "sync", "copy", 1, 6, 1, log,
        std::chrono::milliseconds(50), 2, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "stall", "",
        "", false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 1, 0, 1));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_failure(runtime));
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Timeout);
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}

TEST(RuntimePipelineTest, StopsDuringDescriptorReadinessWithoutPublishingSwitch) {
    const std::string log = unique_log_path("descriptor_stop_during_readiness");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "descriptor_stop_during_readiness", "sync", "copy", 1, 6, 1, log,
        std::chrono::milliseconds(100), 0, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "stall", "",
        "", false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 1, 0, 1));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(
        wait_for_log_prefix(runtime, log, "control storage_readiness_started descriptor=", 1));

    const auto stopped_at = Clock::now();
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_LT(Clock::now() - stopped_at, std::chrono::milliseconds(500));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Timeout);
    auto secondary = runtime.secondary_failures();
    ASSERT_TRUE(secondary.has_value());
    EXPECT_TRUE(secondary->empty());
    EXPECT_EQ(runtime.secondary_failure_overflow_count(), 0U);
    EXPECT_EQ(log_count(log, "control prepare transition="), 1U);
    EXPECT_TRUE(log_contains(log, "old=0"));
}

TEST(RuntimePipelineTest, BoundsAcquisitionBindingControlDuringSwitch) {
    for (const std::string binding_scenario :
         {"prepare_stall_switch", "commit_stall_switch", "prepare_failure_abort_stall"}) {
        const std::string log = unique_log_path("binding_control_stall");
        std::error_code removed;
        fs::remove(log, removed);
        Runtime runtime(options(
            binding_scenario, "sync", "copy", 1, 6, 1, log, std::chrono::milliseconds(50), 2, 16,
            64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", binding_scenario));
        ASSERT_TRUE(runtime.configure().has_value());
        ASSERT_TRUE(runtime.start().has_value());
        ASSERT_TRUE(wait_for_failure(runtime));
        EXPECT_EQ(runtime.state(), RuntimeState::Failed);
        const auto started = Clock::now();
        EXPECT_FALSE(runtime.stop().has_value());
        EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(500));
    }
}

TEST(RuntimePipelineTest, RejectsCanonicalRegistrationCapacityExhaustion) {
    const std::string log = unique_log_path("registration_capacity");
    std::error_code removed;
    fs::remove(log, removed);
    auto runtime_options = options("registration_capacity", "sync", "copy", 2, 1, 1, log);
    runtime_options.resources.max_streams = 1;
    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.configure().has_value());
    EXPECT_FALSE(runtime.start().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}

TEST(RuntimePipelineTest, EnforcesNegotiatedStorageInflightCapacity) {
    const std::string log = unique_log_path("storage_capacity");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "storage_capacity", "sync", "copy", 1, 8, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 2, 32));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 8));
    ASSERT_TRUE(runtime.stop().has_value());
    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 8U);
    EXPECT_EQ(snapshot->inflight_records, 0U);
    EXPECT_EQ(record_count(log), 8U);

    const std::string invalid_log = unique_log_path("invalid_storage_capacity");
    std::error_code invalid_removed;
    fs::remove(invalid_log, invalid_removed);
    Runtime invalid(options(
        "invalid_storage_capacity", "sync", "copy", 1, 1, 1, invalid_log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "",
        "", false, "", 1000, 0, 32));
    ASSERT_TRUE(invalid.configure().has_value());
    EXPECT_FALSE(invalid.start().has_value());
    EXPECT_EQ(invalid.state(), RuntimeState::Failed);
}

TEST(RuntimePipelineTest, ReservesCompletionCapacityForEveryAdmittedOperation) {
    struct Scenario {
        const char* name;
        int records;
        std::size_t completion_capacity;
    };
    const Scenario scenarios[] = {
        {"completion_capacity_two", 12, 2},
        {"completion_capacity_one", 4, 1},
    };
    for (const Scenario& scenario : scenarios) {
        const std::string log = unique_log_path(scenario.name);
        std::error_code removed;
        fs::remove(log, removed);
        Runtime runtime(options(
            scenario.name, "async_payload_async_persistence", "retain", 1, scenario.records, 1, log,
            std::chrono::seconds(1), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "",
            "", false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 1,
            scenario.completion_capacity));
        ASSERT_TRUE(runtime.configure().has_value());
        ASSERT_TRUE(runtime.start().has_value());
        ASSERT_TRUE(wait_for_records(runtime, log, static_cast<std::size_t>(scenario.records)));
        ASSERT_TRUE(runtime.stop().has_value());
        const auto snapshot = runtime.stats();
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->accepted_records, static_cast<std::uint64_t>(scenario.records));
        EXPECT_EQ(snapshot->inflight_records, 0U);
        EXPECT_EQ(snapshot->inflight_bytes, 0U);
        EXPECT_LE(snapshot->high_water_records, scenario.completion_capacity);
        EXPECT_EQ(record_count(log), static_cast<std::size_t>(scenario.records));
    }
}

TEST(RuntimePipelineTest, ReconcilesEarlyCrossThreadPayloadCompletion) {
    const auto log = unique_log_path("early");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "early", "early_cross_thread", "copy", 1, 4, 1, log, std::chrono::milliseconds(500)));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 4));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Stopped);
}

TEST(RuntimePipelineTest, HandlesZeroLengthAndLargePayloads) {
    const std::string zero_log = unique_log_path("zero_payload");
    std::error_code zero_removed;
    fs::remove(zero_log, zero_removed);
    Runtime zero_runtime(options(
        "zero_payload", "sync", "copy", 1, 1, 1, zero_log, std::chrono::milliseconds(500), -1, 0));
    ASSERT_TRUE(zero_runtime.configure().has_value());
    ASSERT_TRUE(zero_runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(zero_runtime, zero_log, 1));
    ASSERT_TRUE(zero_runtime.stop().has_value());

    const std::string log = unique_log_path("large_payload");
    std::error_code removed;
    fs::remove(log, removed);
    constexpr std::int64_t kLargeBytes = 2700 * 1024;
    Runtime runtime(options(
        "large_payload", "sync", "copy", 1, 1, 1, log, std::chrono::milliseconds(500), -1,
        kLargeBytes, 3U * 1024U * 1024U));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());
    const auto lines = read_lines(log);
    const auto record = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
        return line.rfind("record ", 0) == 0;
    });
    ASSERT_NE(record, lines.end());
    EXPECT_NE(record->find("bytes=2764800"), std::string::npos);
}

TEST(RuntimePipelineTest, StopDuringActiveProducerConverges) {
    const auto log = unique_log_path("stop_active");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "stop_active", "async_payload_async_persistence", "retain", 2, 200, 4, log,
        std::chrono::milliseconds(500)));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Stopped);
}

TEST(RuntimePipelineTest, DetectsSameStackCompletionAndFailsClosed) {
    const auto log = unique_log_path("same_stack");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(
        options("same_stack", "same_stack", "copy", 1, 1, 1, log, std::chrono::milliseconds(100)));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (runtime.state() != RuntimeState::Failed && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_TRUE(runtime.primary_failure().has_value());
}

TEST(RuntimePipelineTest, DetectsCompletionBeforeNonOkReturn) {
    expect_failed_scenario(
        "callback_before_error", "callback_before_error", std::chrono::milliseconds(100));
}

TEST(RuntimePipelineTest, DetectsDuplicatePayloadCompletion) {
    expect_failed_scenario(
        "duplicate_payload", "duplicate_payload", std::chrono::milliseconds(100));
}

TEST(RuntimePipelineTest, DetectsDuplicatePersistenceCompletion) {
    expect_failed_scenario(
        "duplicate_persistence", "duplicate_persistence", std::chrono::milliseconds(100));
}

TEST(RuntimePipelineTest, DetectsAdditionalCompletionContractViolations) {
    expect_failed_scenario(
        "sync_persistence_callback", "sync_persistence_callback", std::chrono::milliseconds(100));
    expect_failed_scenario(
        "unknown_operation", "unknown_operation", std::chrono::milliseconds(100));
    expect_failed_scenario(
        "missing_diagnostic", "missing_diagnostic", std::chrono::milliseconds(100));
    expect_failed_scenario(
        "invalid_persistence_state", "invalid_persistence_state", std::chrono::milliseconds(100));
    expect_failed_scenario(
        "malformed_callback_diagnostic", "malformed_callback_diagnostic",
        std::chrono::milliseconds(100));
}

TEST(RuntimePipelineTest, MissingCallbacksDoNotHangStop) {
    expect_failed_scenario("missing_callback", "missing_callback", std::chrono::milliseconds(100));
}

TEST(RuntimePipelineTest, CallbackAfterCloseFailsClosed) {
    expect_failed_scenario(
        "callback_after_close", "callback_after_close", std::chrono::milliseconds(100));
}

TEST(RuntimePipelineTest, PreservesAcceptedNoDropUnderDispatchSaturation) {
    const std::string wait_log = unique_log_path("saturated_wait");
    std::error_code wait_removed;
    fs::remove(wait_log, wait_removed);
    Runtime wait_runtime(options(
        "saturated_wait", "sync", "copy", 1, 8, 1, wait_log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 2, AdmissionMode::Wait, 10'000));
    ASSERT_TRUE(wait_runtime.configure().has_value());
    ASSERT_TRUE(wait_runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(wait_runtime, wait_log, "submit ", 8));
    ASSERT_TRUE(wait_runtime.stop().has_value());
    const auto wait_stats = wait_runtime.stats();
    ASSERT_TRUE(wait_stats.has_value());
    EXPECT_EQ(wait_stats->accepted_records, 8U);
    EXPECT_EQ(wait_stats->rejected_records, 0U);
    EXPECT_EQ(wait_stats->inflight_records, 0U);
    EXPECT_EQ(wait_stats->inflight_bytes, 0U);
    EXPECT_EQ(record_count(wait_log), 8U);

    const std::string reject_log = unique_log_path("saturated_reject");
    std::error_code reject_removed;
    fs::remove(reject_log, reject_removed);
    Runtime reject_runtime(options(
        "saturated_reject", "sync", "copy", 1, 8, 1, reject_log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 2, AdmissionMode::Reject, 50'000));
    ASSERT_TRUE(reject_runtime.configure().has_value());
    ASSERT_TRUE(reject_runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(reject_runtime, reject_log, "submit ", 8));
    ASSERT_TRUE(reject_runtime.stop().has_value());
    const auto reject_stats = reject_runtime.stats();
    ASSERT_TRUE(reject_stats.has_value());
    EXPECT_EQ(reject_stats->accepted_records, 2U);
    EXPECT_EQ(reject_stats->rejected_records, 6U);
    EXPECT_EQ(reject_stats->timed_out_records, 0U);
    EXPECT_EQ(reject_stats->gate_closed_records, 0U);
    EXPECT_EQ(reject_stats->inflight_records, 0U);
    EXPECT_EQ(reject_stats->inflight_bytes, 0U);
    EXPECT_EQ(record_count(reject_log), 2U);
    EXPECT_TRUE(log_contains(reject_log, "outcome=1"));
    EXPECT_TRUE(log_contains(reject_log, "reason=12"));
}

TEST(RuntimePipelineTest, ReportsPayloadDispositionIndependentlyFromRejection) {
    const std::string log = unique_log_path("payload_disposition");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "payload_disposition", "sync", "adopt", 1, 4, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 1, AdmissionMode::Reject, 50'000));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 4));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(log_contains(log, "outcome=3 reason=0 disposition=2"));
    EXPECT_TRUE(log_contains(log, "outcome=1 reason=12 disposition=1"));
}

TEST(RuntimePipelineTest, ReportsBlockedAdmissionWaitTimeout) {
    const std::string log = unique_log_path("admission_wait_timeout");
    std::error_code removed;
    fs::remove(log, removed);
    auto runtime_options = options(
        "admission_wait_timeout", "sync", "copy", 1, 2, 1, log, std::chrono::seconds(3), -1, 16,
        64U * 1024U, 1, AdmissionMode::Wait, 1'000'000);
    runtime_options.submit_wait_timeout = std::chrono::milliseconds(100);
    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 2));
    ASSERT_TRUE(runtime.stop().has_value());
    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 1U);
    EXPECT_EQ(snapshot->timed_out_records, 1U);
    EXPECT_EQ(record_count(log), 1U);
}

TEST(RuntimePipelineTest, BoundsStorageSubmitFlushAndCloseStalls) {
    struct Scenario {
        const char* name;
        const char* storage_scenario;
    };
    const Scenario scenarios[] = {
        {"submit_stall", "storage_stall"},
        {"flush_stall", "flush_stall"},
        {"close_stall", "close_stall"},
    };
    for (const Scenario& scenario : scenarios) {
        const std::string log = unique_log_path(scenario.name);
        std::error_code removed;
        fs::remove(log, removed);
        Runtime runtime(options(
            scenario.name, scenario.storage_scenario, "copy", 1, 1, 1, log,
            std::chrono::milliseconds(50)));
        ASSERT_TRUE(runtime.configure().has_value());
        const auto start_result = runtime.start();
        ASSERT_TRUE(start_result.has_value())
            << (start_result.has_value() ? std::string_view{} : start_result.error().message());
        ASSERT_TRUE(wait_for_records(runtime, log, 1));

        const auto started = Clock::now();
        EXPECT_FALSE(runtime.stop().has_value());
        const auto elapsed = Clock::now() - started;
        EXPECT_LT(elapsed, std::chrono::milliseconds(500));
        EXPECT_EQ(runtime.state(), RuntimeState::Failed);
        auto primary = runtime.primary_failure();
        ASSERT_TRUE(primary.has_value());
        EXPECT_EQ(primary->code(), ErrorCode::Timeout);
        EXPECT_FALSE(runtime.stop().has_value());
        EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    }
}

TEST(RuntimePipelineTest, DetectsStorageHandoffStallWithoutCallingStop) {
    const std::string log = unique_log_path("handoff_stall_watchdog");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "handoff_stall_watchdog", "storage_stall", "copy", 1, 1, 1, log,
        std::chrono::milliseconds(50)));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(wait_for_failure(runtime));
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Timeout);
    const auto started = Clock::now();
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(500));
}

TEST(RuntimePipelineTest, KeepsRunningAcrossMultipleHandoffTimeoutPeriods) {
    constexpr int kRecords = 80;
    const std::string log = unique_log_path("continuous_handoff");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "continuous_handoff", "sync", "copy", 1, kRecords, 1, log, std::chrono::milliseconds(100),
        -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 5'000));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, static_cast<std::size_t>(kRecords)));
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_FALSE(runtime.primary_failure().has_value());
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Stopped);
    EXPECT_EQ(record_count(log), static_cast<std::size_t>(kRecords));
}

TEST(RuntimePipelineTest, BoundsAcquisitionQuiesceAndStopFailures) {
    struct Scenario {
        const char* name;
        const char* lifecycle_scenario;
    };
    const Scenario scenarios[] = {
        {"quiesce_failure", "quiesce_failure"},
        {"quiesce_stall", "quiesce_stall"},
        {"stop_failure", "stop_failure"},
        {"stop_stall", "stop_stall"},
    };
    for (const Scenario& scenario : scenarios) {
        const std::string log = unique_log_path(scenario.name);
        std::error_code removed;
        fs::remove(log, removed);
        Runtime runtime(options(
            scenario.name, "sync", "copy", 1, 1, 1, log, std::chrono::milliseconds(50), -1, 16,
            64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", scenario.lifecycle_scenario));
        ASSERT_TRUE(runtime.configure().has_value());
        ASSERT_TRUE(runtime.start().has_value());
        ASSERT_TRUE(wait_for_records(runtime, log, 1));

        const auto started = Clock::now();
        EXPECT_FALSE(runtime.stop().has_value());
        const auto elapsed = Clock::now() - started;
        EXPECT_LT(elapsed, std::chrono::milliseconds(500));
        EXPECT_EQ(runtime.state(), RuntimeState::Failed);
        auto primary = runtime.primary_failure();
        ASSERT_TRUE(primary.has_value());
        if (std::string_view(scenario.lifecycle_scenario).find("stall") != std::string_view::npos) {
            EXPECT_EQ(primary->code(), ErrorCode::Timeout);
        } else {
            EXPECT_EQ(primary->code(), ErrorCode::Unavailable);
        }
    }
}

TEST(RuntimePipelineTest, RollsBackPreviouslyStartedAcquisitionsInReverseOrder) {
    const std::string log = unique_log_path("multi_acquisition_rollback");
    std::error_code removed;
    fs::remove(log, removed);
    auto runtime_options = options("multi_acquisition_rollback", "sync", "copy", 1, 1, 1, log);
    runtime_options.acquisitions.clear();
    for (const std::string lifecycle_scenario : {"", "start_partial_failure"}) {
        PluginConfig acquisition;
        acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
        acquisition.config = map_config({
            {"source_id",
             plugin::ConfigTree::string(
                 "fake-source-" + std::to_string(runtime_options.acquisitions.size()))},
            {"streams", plugin::ConfigTree::int64(1)},
            {"records_per_stream", plugin::ConfigTree::int64(1)},
            {"producer_threads", plugin::ConfigTree::int64(1)},
            {"payload_size", plugin::ConfigTree::int64(16)},
            {"payload_mode", plugin::ConfigTree::string("copy")},
            {"descriptor_change_after", plugin::ConfigTree::int64(-1)},
            {"binding_scenario", plugin::ConfigTree::string("")},
            {"lifecycle_scenario", plugin::ConfigTree::string(lifecycle_scenario)},
            {"pause_before_switch", plugin::ConfigTree::int64(0)},
            {"stale_handle_file", plugin::ConfigTree::string("")},
            {"projection_id", plugin::ConfigTree::string("")},
            {"projection_before_start", plugin::ConfigTree::int64(0)},
            {"projection_delay_after", plugin::ConfigTree::int64(-1)},
            {"projection_registration_mode", plugin::ConfigTree::string("")},
            {"timestamp_scenario", plugin::ConfigTree::string("")},
            {"timestamp_key_count", plugin::ConfigTree::int64(1)},
            {"event_log", plugin::ConfigTree::string(log)},
        });
        runtime_options.acquisitions.push_back(std::move(acquisition));
    }

    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.configure().has_value());
    EXPECT_FALSE(runtime.start().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_NE(primary->message().find("partial start failure"), std::string_view::npos);
    auto secondary = runtime.secondary_failures();
    ASSERT_TRUE(secondary.has_value());
    EXPECT_TRUE(secondary->empty())
        << (secondary->empty() ? std::string_view{} : secondary->front().message());
    EXPECT_GE(log_count(log, "control quiesce"), 2U);
    EXPECT_GE(log_count(log, "control stop"), 2U);
}

TEST(RuntimePipelineTest, PreservesPartialStartAndCleanupFailures) {
    const std::string log = unique_log_path("partial_start_cleanup_failure");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "partial_start_cleanup_failure", "sync", "copy", 1, 1, 1, log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "",
        "start_partial_cleanup_failure"));
    ASSERT_TRUE(runtime.configure().has_value());
    EXPECT_FALSE(runtime.start().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_NE(primary->message().find("partial start failure"), std::string_view::npos);
    auto secondary = runtime.secondary_failures();
    ASSERT_TRUE(secondary.has_value());
    ASSERT_FALSE(secondary->empty());
    EXPECT_NE(secondary->front().message().find("quiesce failure"), std::string_view::npos);
}

TEST(RuntimePipelineTest, PreservesPrimaryAndSecondaryFailureDiagnostics) {
    const std::string log = unique_log_path("combined_failure");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "combined_failure", "flush_and_close_failure", "copy", 1, 1, 1, log,
        std::chrono::milliseconds(100)));
    ASSERT_TRUE(runtime.configure().has_value());
    const auto start_result = runtime.start();
    ASSERT_TRUE(start_result.has_value())
        << (start_result.has_value() ? std::string_view{} : start_result.error().message());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);

    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::StorageError);
    EXPECT_NE(primary->message().find("flush failure"), std::string_view::npos);
    auto secondary = runtime.secondary_failures();
    ASSERT_TRUE(secondary.has_value());
    ASSERT_EQ(secondary->size(), 1U);
    EXPECT_EQ(secondary->front().code(), ErrorCode::StorageError);
    EXPECT_NE(secondary->front().message().find("close failure"), std::string_view::npos);
    EXPECT_EQ(runtime.secondary_failure_overflow_count(), 0U);
}

TEST(RuntimePipelineTest, FailsClosedOnStorageSubmitAndBackgroundFailures) {
    const std::string submit_log = unique_log_path("submit_error");
    std::error_code submit_removed;
    fs::remove(submit_log, submit_removed);
    Runtime submit_runtime(options("submit_error", "submit_error", "copy", 1, 1, 1, submit_log));
    ASSERT_TRUE(submit_runtime.configure().has_value());
    ASSERT_TRUE(submit_runtime.start().has_value());
    ASSERT_TRUE(wait_for_failure(submit_runtime));
    EXPECT_FALSE(submit_runtime.stop().has_value());
    EXPECT_EQ(submit_runtime.state(), RuntimeState::Failed);

    const std::string background_log = unique_log_path("background_fatal");
    std::error_code background_removed;
    fs::remove(background_log, background_removed);
    Runtime background_runtime(
        options("background_fatal", "background_fatal", "copy", 1, 1, 1, background_log));
    ASSERT_TRUE(background_runtime.configure().has_value());
    ASSERT_TRUE(background_runtime.start().has_value());
    ASSERT_TRUE(wait_for_failure(background_runtime));
    EXPECT_FALSE(background_runtime.stop().has_value());
    auto primary = background_runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_NE(primary->message().find("background failure"), std::string_view::npos);
}

TEST(RuntimePipelineTest, RejectsStaleHandlesAcrossActivations) {
    const std::string log = unique_log_path("stale_handles");
    const std::string handle_file = unique_log_path("stale_handles_fixture");
    std::error_code removed;
    fs::remove(log, removed);
    fs::remove(handle_file, removed);
    Runtime runtime(options(
        "stale_handles", "sync", "copy", 1, 1, 1, log, std::chrono::seconds(1), -1, 16, 64U * 1024U,
        8, AdmissionMode::Wait, 0, "ready", "", "", false, handle_file));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(fs::is_regular_file(handle_file));

    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 2));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(log_contains(log, "stale_handles_rejected"));
}

TEST(RuntimePipelineTest, QualifiesMixedPayloadModesInOneActivation) {
    const std::string log = unique_log_path("mixed_payload_modes");
    std::error_code removed;
    fs::remove(log, removed);
    auto runtime_options = options("mixed_payload_modes", "sync", "copy", 1, 2, 1, log);
    runtime_options.acquisitions.clear();
    for (const std::string payload_mode : {"copy", "adopt", "retain"}) {
        PluginConfig acquisition;
        acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
        acquisition.config = map_config({
            {"source_id",
             plugin::ConfigTree::string(
                 "fake-source-" + std::to_string(runtime_options.acquisitions.size()))},
            {"streams", plugin::ConfigTree::int64(1)},
            {"records_per_stream", plugin::ConfigTree::int64(2)},
            {"producer_threads", plugin::ConfigTree::int64(1)},
            {"payload_size", plugin::ConfigTree::int64(16)},
            {"payload_mode", plugin::ConfigTree::string(payload_mode)},
            {"descriptor_change_after", plugin::ConfigTree::int64(-1)},
            {"binding_scenario", plugin::ConfigTree::string("")},
            {"lifecycle_scenario", plugin::ConfigTree::string("")},
            {"pause_before_switch", plugin::ConfigTree::int64(0)},
            {"stale_handle_file", plugin::ConfigTree::string("")},
            {"event_log", plugin::ConfigTree::string(log)},
        });
        runtime_options.acquisitions.push_back(std::move(acquisition));
    }

    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 6));
    ASSERT_TRUE(runtime.stop().has_value());
    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 6U);
    EXPECT_EQ(snapshot->accepted_bytes, 96U);
    EXPECT_EQ(snapshot->inflight_records, 0U);
    EXPECT_EQ(record_count(log), 6U);
}

TEST(RuntimePipelineTest, IsolatesAcquisitionOwnedObjectsAcrossPlugins) {
    for (const std::string mode : {"copy", "adopt", "retain"}) {
        const std::string log = unique_log_path("ownership_" + mode);
        const std::string handles = log + ".handles";
        std::error_code removed;
        fs::remove(log, removed);
        fs::remove(handles, removed);
        auto config = options("ownership_" + mode, "sync", mode, 1, 3, 1, log);
        config.acquisitions.clear();
        for (const std::string scenario : {"owner", "probe"}) {
            PluginConfig acquisition;
            acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
            acquisition.config = map_config({
                {"source_id", plugin::ConfigTree::string("source-" + scenario)},
                {"ownership_scenario", plugin::ConfigTree::string(scenario)},
                {"ownership_file", plugin::ConfigTree::string(handles)},
                {"records_per_stream", plugin::ConfigTree::int64(3)},
                {"payload_mode", plugin::ConfigTree::string(mode)},
                {"event_log", plugin::ConfigTree::string(log)},
            });
            config.acquisitions.push_back(std::move(acquisition));
        }
        Runtime runtime(std::move(config));
        ASSERT_TRUE(runtime.configure().has_value());
        ASSERT_TRUE(runtime.start().has_value()) << mode;
        ASSERT_TRUE(wait_for_records(runtime, log, 6)) << mode;
        ASSERT_TRUE(runtime.stop().has_value());
        EXPECT_TRUE(log_contains(log, "ownership_control_rejected"));
        EXPECT_EQ(log_count(log, "ownership_submit_rejected"), 3U);
        auto stats = runtime.stats();
        ASSERT_TRUE(stats.has_value());
        EXPECT_EQ(stats->accepted_records, 6U);
        EXPECT_EQ(stats->rejected_records, 3U);
        EXPECT_EQ(stats->inflight_records, 0U);
        fs::remove(handles, removed);
    }
}

TEST(RuntimePipelineTest, RejectsCrossPluginSourceIdentityCollisionDuringStart) {
    const std::string log = unique_log_path("source_collision");
    std::error_code removed;
    fs::remove(log, removed);
    auto config = options("source_collision", "sync", "copy", 1, 1, 1, log);
    config.acquisitions.push_back(config.acquisitions.front());
    Runtime runtime(std::move(config));
    ASSERT_TRUE(runtime.configure().has_value());
    EXPECT_FALSE(runtime.start().has_value());
    ASSERT_TRUE(runtime.primary_failure().has_value());
    EXPECT_EQ(runtime.primary_failure()->code(), ErrorCode::Conflict);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
}

TEST(RuntimePipelineTest, PluginRecoveryRequiresExplicitSourceRecoveryAndIsOwnerScoped) {
    const std::string log = unique_log_path("source_recovery");
    const std::string recovered_file = log + ".recovered";
    std::error_code removed;
    fs::remove(log, removed);
    fs::remove(recovered_file, removed);
    auto config = options("source_recovery", "sync", "copy", 1, 3, 1, log);
    config.acquisitions.clear();
    for (const std::string scenario : {"source_recovered", ""}) {
        PluginConfig acquisition;
        acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
        acquisition.config = map_config({
            {"producer_wait_file",
             plugin::ConfigTree::string(scenario.empty() ? recovered_file : "")},
            {"plugin_health_file",
             plugin::ConfigTree::string(scenario.empty() ? "" : recovered_file)},
            {"source_id", plugin::ConfigTree::string("source-" + scenario)},
            {"records_per_stream", plugin::ConfigTree::int64(3)},
            {"plugin_health_after", plugin::ConfigTree::int64(scenario.empty() ? -1 : 0)},
            {"plugin_health_scenario", plugin::ConfigTree::string(scenario)},
            {"event_log", plugin::ConfigTree::string(log)},
        });
        config.acquisitions.push_back(std::move(acquisition));
    }
    Runtime runtime(std::move(config));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 5));
    ASSERT_TRUE(runtime.stop().has_value());
    EXPECT_TRUE(log_contains(log, "source_explicitly_recovered"));
    auto stats = runtime.stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->accepted_records, 5U);
    EXPECT_EQ(stats->rejected_records, 1U);
    fs::remove(recovered_file, removed);
}

TEST(RuntimePipelineTest, StressConcurrentAdmissionAndDescriptorTransitions) {
    constexpr int kStreams = 4;
    constexpr int kRecordsPerStream = 100;
    constexpr int kProducers = 4;
    constexpr std::size_t kSubmissions =
        static_cast<std::size_t>(kStreams * kRecordsPerStream * kProducers);
    const std::string log = unique_log_path("concurrent_stress");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "concurrent_stress", "sync", "copy", kStreams, kRecordsPerStream, kProducers, log,
        std::chrono::seconds(3), 50, 1024, 2U * 1024U * 1024U, 16));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", kSubmissions));
    ASSERT_TRUE(runtime.stop().has_value());

    const auto snapshot = runtime.stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_GT(snapshot->accepted_records, 0U);
    EXPECT_EQ(snapshot->inflight_records, 0U);
    EXPECT_EQ(snapshot->inflight_bytes, 0U);
    EXPECT_EQ(record_count(log), snapshot->accepted_records);
    EXPECT_EQ(snapshot->accepted_bytes, snapshot->accepted_records * 1024U);

    std::unordered_map<std::uint64_t, std::uint64_t> last_sequence;
    for (const std::string& line : read_lines(log)) {
        if (line.rfind("record ", 0) != 0) {
            continue;
        }
        std::istringstream input(line);
        std::string token;
        std::uint64_t stream = 0;
        std::uint64_t sequence = 0;
        while (input >> token) {
            if (token.rfind("stream=", 0) == 0) {
                stream = std::stoull(token.substr(7));
            } else if (token.rfind("sequence=", 0) == 0) {
                sequence = std::stoull(token.substr(9));
            }
        }
        const auto inserted = last_sequence.emplace(stream, sequence);
        if (!inserted.second) {
            EXPECT_GT(sequence, inserted.first->second);
            inserted.first->second = sequence;
        }
    }
}

TEST(RuntimePipelineTest, RestartCreatesFreshActivation) {
    const auto log = unique_log_path("restart");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("restart", "sync", "copy", 1, 1, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.stop().has_value());

    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 2));
    ASSERT_TRUE(runtime.stop().has_value());
    const auto lines = read_lines(log);
    EXPECT_EQ(
        std::count_if(
            lines.begin(), lines.end(),
            [](const std::string& line) { return line.rfind("record ", 0) == 0; }),
        2);
}

TEST(RuntimePipelineTest, RejectsStartWhenActivationRootBudgetIsExhausted) {
    auto& quarantine = QuarantineManager::process();
    std::vector<std::uint64_t> reserved;
    while (true) {
        auto slot = quarantine.reserve_activation("Runtime activation budget test");
        if (!slot.has_value()) {
            ASSERT_EQ(slot.error().code(), ErrorCode::ResourceExhausted);
            break;
        }
        reserved.push_back(slot.value());
        ASSERT_LE(reserved.size(), 32U);
    }

    const std::string log = unique_log_path("activation_budget");
    Runtime runtime(options("activation_budget", "sync", "copy", 1, 1, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    const auto started = runtime.start();
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::ResourceExhausted);
    EXPECT_EQ(runtime.state(), RuntimeState::Configured);

    for (const std::uint64_t slot : reserved) {
        quarantine.release_activation(slot);
    }
}

TEST(RuntimePipelineTest, RetainsBlockedPayloadReleaseWorkerDuringStop) {
    const std::string log = unique_log_path("blocked_release");
    const std::string release_file = unique_log_path("blocked_release_fixture");
    std::error_code removed;
    fs::remove(log, removed);
    fs::remove(release_file + ".blocked", removed);
    fs::remove(release_file + ".continue", removed);

    auto runtime_options = options(
        "blocked_release", "sync_payload_async_persistence", "adopt", 1, 1, 1, log,
        std::chrono::milliseconds(80));
    runtime_options.acquisitions[0].config = map_config({
        {"streams", plugin::ConfigTree::int64(1)},
        {"records_per_stream", plugin::ConfigTree::int64(1)},
        {"producer_threads", plugin::ConfigTree::int64(1)},
        {"payload_size", plugin::ConfigTree::int64(16)},
        {"payload_mode", plugin::ConfigTree::string("adopt")},
        {"payload_release_block_file", plugin::ConfigTree::string(release_file)},
        {"event_log", plugin::ConfigTree::string(log)},
    });

    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_file(release_file + ".blocked"));

    const std::size_t retained_before = QuarantineManager::process().retained_worker_count();
    const auto stop_started = Clock::now();
    const auto stopped = runtime.stop();
    const auto stop_elapsed = Clock::now() - stop_started;
    const std::size_t retained_after = QuarantineManager::process().retained_worker_count();

    std::ofstream continue_file(release_file + ".continue", std::ios::binary | std::ios::trunc);
    continue_file.put('\0');
    continue_file.close();

    EXPECT_FALSE(stopped.has_value());
    EXPECT_LT(stop_elapsed, std::chrono::milliseconds(500));
    EXPECT_GT(retained_after, retained_before);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);

    const auto reap_deadline = Clock::now() + std::chrono::seconds(2);
    while (QuarantineManager::process().retained_worker_count() != retained_before &&
           Clock::now() < reap_deadline) {
        QuarantineManager::process().reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(QuarantineManager::process().retained_worker_count(), retained_before);
}

TEST(RuntimePipelineTest, QuarantinedReleaseOutlivesRuntimeAndJoinsBeforeRootRecovery) {
    const std::string log = unique_log_path("release_after_runtime_destroy");
    const std::string marker = unique_log_path("release_after_runtime_destroy_fixture");
    std::error_code removed;
    fs::remove(log, removed);
    fs::remove(marker + ".blocked", removed);
    fs::remove(marker + ".continue", removed);
    struct UnblockRelease {
        std::string marker;
        ~UnblockRelease() { std::ofstream(marker + ".continue").put('1'); }
    } unblock{marker};
    auto& manager = QuarantineManager::process();
    const auto workers_before = manager.retained_worker_count();
    const auto roots_before = manager.retained_activation_count();
    auto config = options(
        "release_after_destroy", "sync", "adopt", 1, 1, 1, log, std::chrono::milliseconds(60));
    config.acquisitions[0].config = map_config({
        {"streams", plugin::ConfigTree::int64(1)},
        {"records_per_stream", plugin::ConfigTree::int64(1)},
        {"producer_threads", plugin::ConfigTree::int64(1)},
        {"payload_size", plugin::ConfigTree::int64(16)},
        {"payload_mode", plugin::ConfigTree::string("adopt")},
        {"payload_release_block_file", plugin::ConfigTree::string(marker)},
        {"event_log", plugin::ConfigTree::string(log)},
    });
    auto runtime = std::make_unique<Runtime>(std::move(config));
    ASSERT_TRUE(runtime->configure().has_value());
    ASSERT_TRUE(runtime->start().has_value());
    ASSERT_TRUE(wait_for_file(marker + ".blocked"));
    EXPECT_FALSE(runtime->stop().has_value());
    EXPECT_GT(manager.retained_worker_count(), workers_before);
    runtime.reset();  // No application observer keeps the Pipeline alive now.
    std::ofstream(marker + ".continue").put('1');
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while ((manager.retained_worker_count() != workers_before ||
            manager.retained_activation_count() > roots_before ||
            !log_contains(log, "control storage_destroy") ||
            !log_contains(log, "control destroy")) &&
           Clock::now() < deadline) {
        manager.reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(manager.retained_worker_count(), workers_before);
    EXPECT_LE(manager.retained_activation_count(), roots_before);
    EXPECT_TRUE(log_contains(log, "control storage_destroy"));
    EXPECT_TRUE(log_contains(log, "control destroy"));
}

TEST(RuntimePipelineTest, QuarantinesBlockedPluginDestroyDuringStop) {
    const std::string log = unique_log_path("blocked_destroy");
    const std::string destroy_file = unique_log_path("blocked_destroy_fixture");
    std::error_code removed;
    fs::remove(log, removed);
    fs::remove(destroy_file + ".blocked", removed);
    fs::remove(destroy_file + ".continue", removed);

    auto runtime_options =
        options("blocked_destroy", "sync", "copy", 1, 1, 1, log, std::chrono::milliseconds(80));
    runtime_options.acquisitions[0].config = map_config({
        {"streams", plugin::ConfigTree::int64(1)},
        {"records_per_stream", plugin::ConfigTree::int64(1)},
        {"producer_threads", plugin::ConfigTree::int64(1)},
        {"payload_size", plugin::ConfigTree::int64(16)},
        {"payload_mode", plugin::ConfigTree::string("copy")},
        {"destroy_block_file", plugin::ConfigTree::string(destroy_file)},
        {"event_log", plugin::ConfigTree::string(log)},
    });

    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));

    const std::size_t retained_before = QuarantineManager::process().retained_call_count();
    const auto stop_started = Clock::now();
    const auto stopped = runtime.stop();
    const auto stop_elapsed = Clock::now() - stop_started;
    const std::size_t retained_after = QuarantineManager::process().retained_call_count();

    std::ofstream continue_file(destroy_file + ".continue", std::ios::binary | std::ios::trunc);
    continue_file.put('\0');
    continue_file.close();

    EXPECT_FALSE(stopped.has_value());
    EXPECT_LT(stop_elapsed, std::chrono::milliseconds(500));
    EXPECT_GT(retained_after, retained_before);
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);

    const auto reap_deadline = Clock::now() + std::chrono::seconds(2);
    while (QuarantineManager::process().retained_call_count() != retained_before &&
           Clock::now() < reap_deadline) {
        QuarantineManager::process().reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(QuarantineManager::process().retained_call_count(), retained_before);
}

std::size_t event_count(const std::string& log, std::string_view event) {
    const auto lines = read_lines(log);
    return static_cast<std::size_t>(std::count(lines.begin(), lines.end(), event));
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = Clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < deadline);
    return predicate();
}

void expect_single_cleanup(const std::string& log, std::string_view close_event) {
    EXPECT_EQ(event_count(log, "control quiesce"), 1U);
    EXPECT_EQ(event_count(log, "control stop"), 1U);
    EXPECT_EQ(event_count(log, "control destroy"), 1U);
    EXPECT_EQ(event_count(log, close_event), 1U);
    EXPECT_EQ(event_count(log, "control storage_destroy"), 1U);
    const auto lines = read_lines(log);
    auto position = [&](std::string_view event) {
        return std::find(lines.begin(), lines.end(), event);
    };
    EXPECT_LT(position("control quiesce"), position("control stop"));
    EXPECT_LT(position("control storage_close disposition=3"), position("control stop"));
    EXPECT_LT(position("control stop"), position("control destroy"));
}

TEST(RuntimePipelineTest, FatalStorageFailureAutomaticallyConvergesWithoutStop) {
    for (const auto* scenario : {"submit_error", "background_fatal"}) {
        const std::string log = unique_log_path(std::string("auto_") + scenario);
        std::error_code removed;
        fs::remove(log, removed);
        Runtime runtime(options("automatic_failure", scenario, "copy", 1, 1, 1, log));
        ASSERT_TRUE(runtime.configure().has_value());
        ASSERT_TRUE(runtime.start().has_value());
        ASSERT_TRUE(eventually([&] { return event_count(log, "control storage_destroy") == 1; }));
        EXPECT_EQ(runtime.state(), RuntimeState::Failed);
        auto primary = runtime.primary_failure();
        ASSERT_TRUE(primary.has_value());
        const auto message = std::string(primary->message());
        EXPECT_FALSE(runtime.stop().has_value());
        expect_single_cleanup(log, "control storage_close disposition=3");
        EXPECT_EQ(runtime.primary_failure()->message(), message);
        auto stats = runtime.stats();
        ASSERT_TRUE(stats.has_value());
        EXPECT_EQ(stats->accepted_records, 1U);
        EXPECT_EQ(stats->inflight_records, 0U);
        EXPECT_EQ(stats->inflight_bytes, 0U);
    }
}

TEST(RuntimePipelineTest, FailureCutoffDoesNotDispatchQueuedRecords) {
    const std::string log = unique_log_path("queued_failure_cutoff");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("queued_failure", "delayed_submit_error", "retain", 1, 3, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(eventually([&] {
        auto stats = runtime.stats();
        return stats.has_value() && stats->accepted_records == 3;
    }));
    ASSERT_TRUE(eventually([&] { return event_count(log, "control storage_destroy") == 1; }));
    EXPECT_EQ(event_count(log, "control storage_submit"), 1U);
    auto stats = runtime.stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->accepted_records, 3U);
    EXPECT_EQ(stats->rejected_records, 0U);
    EXPECT_EQ(stats->persistence_failed_records, 3U);
    EXPECT_EQ(stats->persistence_unknown_records, 0U);
    EXPECT_EQ(stats->inflight_records, 0U);
    EXPECT_EQ(stats->inflight_bytes, 0U);
    expect_single_cleanup(log, "control storage_close disposition=3");
}

TEST(RuntimePipelineTest, HandoffWatchdogAutomaticallyStartsFailureConvergence) {
    const std::string log = unique_log_path("automatic_watchdog");
    std::error_code removed;
    fs::remove(log, removed);
    const auto before = QuarantineManager::process().retained_activation_count();
    Runtime runtime(options(
        "automatic_watchdog", "stall_then_sync", "copy", 1, 3, 1, log,
        std::chrono::milliseconds(60)));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(eventually([&] { return event_count(log, "control quiesce") == 1; }));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    const auto lifecycle = runtime.lifecycle_snapshot();
    ASSERT_TRUE(lifecycle.has_value());
    EXPECT_EQ(lifecycle->normal_acquisition_allowed, RODAQ_FALSE);
    ASSERT_TRUE(eventually([&] { return event_count(log, "control storage_destroy") == 1; }));
    ASSERT_TRUE(eventually(
        [&] { return QuarantineManager::process().retained_activation_count() <= before; }));
    EXPECT_EQ(event_count(log, "control storage_submit"), 1U);
    auto stats = runtime.stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->accepted_records, 3U);
    EXPECT_EQ(stats->persistence_succeeded_records, 1U);
    EXPECT_EQ(stats->persistence_failed_records, 2U);
    EXPECT_EQ(stats->inflight_records, 0U);
    EXPECT_EQ(stats->inflight_bytes, 0U);
    EXPECT_FALSE(runtime.stop().has_value());
    expect_single_cleanup(log, "control storage_close disposition=3");
}

TEST(RuntimePipelineTest, ConcurrentFailureAndStopHaveSingleCleanupOwner) {
    const std::string log = unique_log_path("concurrent_failure_stop");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(
        options("concurrent_failure_stop", "delayed_submit_error", "copy", 1, 3, 1, log));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(eventually([&] { return event_count(log, "control storage_submit") == 1; }));
    std::thread stopper([&] { EXPECT_FALSE(runtime.stop().has_value()); });
    EXPECT_TRUE(eventually([&] { return event_count(log, "control storage_destroy") == 1; }));
    stopper.join();
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.stop().has_value());
    expect_single_cleanup(log, "control storage_close disposition=3");
    EXPECT_EQ(event_count(log, "control storage_submit"), 1U);
}

TEST(RuntimePipelineTest, FailureDuringStartingUsesActivationAbortOnly) {
    const std::string log = unique_log_path("starting_failure_abort_only");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "starting_failure", "sync", "copy", 1, 1, 1, log, std::chrono::milliseconds(500), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "failed"));
    ASSERT_TRUE(runtime.configure().has_value());
    EXPECT_FALSE(runtime.start().has_value());
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.stop().has_value());
    EXPECT_EQ(event_count(log, "control quiesce"), 1U);
    EXPECT_EQ(event_count(log, "control destroy"), 1U);
    EXPECT_EQ(event_count(log, "control storage_destroy"), 1U);
    EXPECT_EQ(event_count(log, "control storage_close disposition=2"), 1U);
    EXPECT_EQ(event_count(log, "control storage_close disposition=3"), 0U);
}

TEST(RuntimePipelineTest, DescriptorWorkerFailureDoesNotSelfJoin) {
    const std::string log = unique_log_path("dynamic_descriptor_failed");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "dynamic_descriptor_failed", "sync", "copy", 1, 3, 1, log, std::chrono::milliseconds(500),
        1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "failed_dynamic"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(eventually([&] { return event_count(log, "control storage_destroy") == 1; }));
    EXPECT_EQ(runtime.state(), RuntimeState::Failed);
    auto primary = runtime.primary_failure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::StorageError);
    EXPECT_NE(primary->message().find("descriptor readiness failure"), std::string_view::npos);
    expect_single_cleanup(log, "control storage_close disposition=3");
}

TEST(RuntimePipelineTest, DynamicUnsupportedDescriptorRemainsStreamLocal) {
    const std::string log = unique_log_path("dynamic_descriptor_unsupported");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "dynamic_descriptor_unsupported", "sync", "copy", 2, 3, 1, log,
        std::chrono::milliseconds(500), 1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0,
        "unsupported_dynamic"));
    ASSERT_TRUE(runtime.configure().has_value());
    ASSERT_TRUE(runtime.start().has_value());
    ASSERT_TRUE(eventually([&] { return event_count(log, "control storage_unsupported") >= 1; }));
    EXPECT_EQ(runtime.state(), RuntimeState::Running);
    EXPECT_FALSE(runtime.primary_failure().has_value());
    EXPECT_TRUE(runtime.stop().has_value());
}

}  // namespace
}  // namespace rodaq::runtime
