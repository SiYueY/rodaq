#include "plugin/config_view.hpp"
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
    auto result = plugin::ConfigTree::Map(std::move(entries));
    if (!result.has_value()) {
        throw std::runtime_error("Failed to construct test config");
    }
    return std::move(result.value());
}

RuntimeOptions options(
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
    int64_t descriptor_stall_after = -1) {
    RuntimeOptions value;
    value.target_uri = "file://fake";
    value.session_id = std::string(name);
    value.payload_block_bytes = block_bytes;
    value.payload_block_count = std::max<std::size_t>(16, capacity);
    value.max_single_payload_bytes = block_bytes;
    value.dispatch_capacity = capacity;
    value.operation_capacity = capacity;
    value.completion_capacity = completion_capacity == 0 ? capacity : completion_capacity;
    value.submit_wait_timeout = std::chrono::milliseconds(500);
    value.stop_progress_timeout = stop_timeout;
    value.admission_mode = admission_mode;
    value.storage.path = required_path("RODAQ_FAKE_STORAGE_PLUGIN");
    value.storage.config = map_config({
        {"scenario", plugin::ConfigTree::String(std::move(storage_scenario))},
        {"event_log", plugin::ConfigTree::String(event_log)},
        {"callback_delay_us", plugin::ConfigTree::Int64(callback_delay_us)},
        {"submit_delay_us", plugin::ConfigTree::Int64(submit_delay_us)},
        {"descriptor_scenario", plugin::ConfigTree::String(std::move(descriptor_scenario))},
        {"descriptor_stall_after", plugin::ConfigTree::Int64(descriptor_stall_after)},
        {"maximum_inflight_records", plugin::ConfigTree::Int64(maximum_inflight_records)},
        {"maximum_inflight_bytes", plugin::ConfigTree::Int64(maximum_inflight_bytes)},
        {"required_projection_id", plugin::ConfigTree::String(std::move(required_projection_id))},
    });
    RuntimePluginOptions acquisition;
    acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
    acquisition.config = map_config({
        {"streams", plugin::ConfigTree::Int64(streams)},
        {"records_per_stream", plugin::ConfigTree::Int64(records)},
        {"producer_threads", plugin::ConfigTree::Int64(producers)},
        {"payload_size", plugin::ConfigTree::Int64(payload_size)},
        {"payload_mode", plugin::ConfigTree::String(std::move(payload_mode))},
        {"descriptor_change_after", plugin::ConfigTree::Int64(descriptor_change_after)},
        {"binding_scenario", plugin::ConfigTree::String(std::move(binding_scenario))},
        {"lifecycle_scenario", plugin::ConfigTree::String(std::move(lifecycle_scenario))},
        {"pause_before_switch", plugin::ConfigTree::Int64(pause_before_switch ? 1 : 0)},
        {"stale_handle_file", plugin::ConfigTree::String(std::move(stale_handle_file))},
        {"projection_id", plugin::ConfigTree::String(std::move(projection_id))},
        {"projection_before_start", plugin::ConfigTree::Int64(projection_before_start ? 1 : 0)},
        {"projection_delay_after", plugin::ConfigTree::Int64(projection_delay_after)},
        {"projection_registration_mode",
         plugin::ConfigTree::String(std::move(projection_registration_mode))},
        {"timestamp_scenario", plugin::ConfigTree::String(std::move(timestamp_scenario))},
        {"timestamp_key_count", plugin::ConfigTree::Int64(timestamp_key_count)},
        {"event_log", plugin::ConfigTree::String(event_log)},
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
        if (runtime.State() == RuntimeState::Failed) {
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
        if (runtime.State() == RuntimeState::Failed) {
            return false;
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, static_cast<std::size_t>(records)));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Stopped);
    EXPECT_EQ(record_count(log), static_cast<std::size_t>(records));
}

void expect_failed_scenario(
    std::string_view name, std::string storage_scenario, std::chrono::milliseconds stop_timeout) {
    const std::string log = unique_log_path(std::string(name));
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(name, std::move(storage_scenario), "copy", 1, 1, 1, log, stop_timeout));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    EXPECT_FALSE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
    EXPECT_TRUE(runtime.PrimaryFailure().has_value());
}

bool wait_for_failure(Runtime& runtime) {
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (runtime.State() != RuntimeState::Failed && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return runtime.State() == RuntimeState::Failed;
}

TEST(RuntimePipelineTest, RejectsInvalidLifecycleTransitions) {
    const auto log = unique_log_path("lifecycle");
    Runtime runtime(options("lifecycle", "sync", "copy", 1, 0, 1, log));
    EXPECT_EQ(runtime.State(), RuntimeState::Constructed);
    EXPECT_FALSE(runtime.Start().has_value());
    ASSERT_TRUE(runtime.Configure().has_value());
    EXPECT_FALSE(runtime.Configure().has_value());
}

TEST(RuntimePipelineTest, CopiesRecordsThroughStableAbiAndStopsCleanly) {
    const auto log = unique_log_path("sync");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("sync", "sync", "copy", 1, 3, 1, log));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_EQ(runtime.State(), RuntimeState::Running);
    ASSERT_TRUE(wait_for_records(runtime, log, 3));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Stopped);

    const auto snapshot = runtime.Stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 3U);
    EXPECT_EQ(snapshot->accepted_bytes, 48U);
    EXPECT_EQ(snapshot->inflight_records, 0U);
    EXPECT_EQ(snapshot->inflight_bytes, 0U);
    EXPECT_GE(snapshot->high_water_records, 1U);
    EXPECT_LE(snapshot->high_water_records, 3U);
    EXPECT_GE(snapshot->high_water_bytes, 16U);
    EXPECT_LE(snapshot->high_water_bytes, 48U);
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

TEST(RuntimePipelineTest, RetainsPayloadThroughAsynchronousDualCompletion) {
    const auto log = unique_log_path("retain_async");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(
        options("retain_async", "async_payload_async_persistence", "retain", 1, 4, 1, log));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 4));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Stopped);
    const auto snapshot = runtime.Stats();
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));

    bool observed_early_release = false;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        const auto snapshot = runtime.Stats();
        if (snapshot.has_value() && snapshot->accepted_records == 1U &&
            snapshot->inflight_records == 1U && snapshot->inflight_bytes == 0U) {
            observed_early_release = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(observed_early_release);
    ASSERT_TRUE(runtime.Stop().has_value());
    const auto final_snapshot = runtime.Stats();
    ASSERT_TRUE(final_snapshot.has_value());
    EXPECT_EQ(final_snapshot->inflight_records, 0U);
    EXPECT_EQ(final_snapshot->inflight_bytes, 0U);
}

TEST(RuntimePipelineTest, PreservesPerStreamSequenceOrderAcrossProducers) {
    const auto log = unique_log_path("multi_stream");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("multi_stream", "sync", "adopt", 2, 5, 2, log));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 20));
    ASSERT_TRUE(runtime.Stop().has_value());

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

TEST(RuntimePipelineTest, SwitchesDescriptorWithoutBreakingStreamOrdering) {
    const auto log = unique_log_path("descriptor_switch");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "descriptor_switch", "sync", "copy", 1, 6, 1, log, std::chrono::milliseconds(500), 2));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 6));
    ASSERT_TRUE(runtime.Stop().has_value());

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
    ASSERT_TRUE(failed.Configure().has_value());
    EXPECT_FALSE(failed.Start().has_value());
    EXPECT_EQ(failed.State(), RuntimeState::Failed);

    const std::string unsupported_log = unique_log_path("readiness_unsupported");
    std::error_code unsupported_removed;
    fs::remove(unsupported_log, unsupported_removed);
    Runtime unsupported(options(
        "readiness_unsupported", "sync", "copy", 1, 1, 1, unsupported_log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0,
        "unsupported"));
    ASSERT_TRUE(unsupported.Configure().has_value());
    ASSERT_TRUE(unsupported.Start().has_value());
    EXPECT_EQ(unsupported.State(), RuntimeState::Running);
    ASSERT_TRUE(unsupported.Stop().has_value());
    EXPECT_EQ(record_count(unsupported_log), 0U);

    const std::string log = unique_log_path("initial_prepare_failure");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "initial_prepare_failure", "sync", "copy", 1, 1, 1, log, std::chrono::milliseconds(100), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "prepare_failure_initial"));
    ASSERT_TRUE(runtime.Configure().has_value());
    EXPECT_FALSE(runtime.Start().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
    EXPECT_TRUE(log_contains(log, "control abort"));
    EXPECT_TRUE(log_contains(log, "disposition=1"));
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.Stop().has_value());
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 2));
    ASSERT_TRUE(runtime.Stop().has_value());
    const auto snapshot = runtime.Stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 1U);
    EXPECT_EQ(snapshot->rejected_records, 1U);
    EXPECT_EQ(record_count(log), 1U);
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 6));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_EQ(record_count(log), 6U);

    const std::string duplicate_log = unique_log_path("projection_duplicate");
    std::error_code duplicate_removed;
    fs::remove(duplicate_log, duplicate_removed);
    Runtime duplicate(options(
        "projection_duplicate", "sync", "copy", 1, 1, 1, duplicate_log, std::chrono::seconds(1), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000,
        128, 128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), true, -1,
        "same"));
    ASSERT_TRUE(duplicate.Configure().has_value());
    ASSERT_TRUE(duplicate.Start().has_value());
    ASSERT_TRUE(wait_for_records(duplicate, duplicate_log, 1));
    ASSERT_TRUE(duplicate.Stop().has_value());

    const std::string conflict_log = unique_log_path("projection_conflict");
    std::error_code conflict_removed;
    fs::remove(conflict_log, conflict_removed);
    Runtime conflict(options(
        "projection_conflict", "sync", "copy", 1, 1, 1, conflict_log, std::chrono::seconds(1), -1,
        16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "require_projection", "", "", false, "", 1000,
        128, 128LL * 1024LL * 1024LL, std::string(kProjection), std::string(kProjection), true, -1,
        "conflict"));
    ASSERT_TRUE(conflict.Configure().has_value());
    EXPECT_FALSE(conflict.Start().has_value());
    auto primary = conflict.PrimaryFailure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Conflict);
}

TEST(RuntimePipelineTest, InjectsMandatoryRuntimeIngressTimestamp) {
    const std::string log = unique_log_path("ingress_timestamp");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options("ingress_timestamp", "sync", "copy", 1, 1, 1, log));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_TRUE(log_contains(log, "timestamp_count=2 ingress_present=1 sample_present=1"));
}

TEST(RuntimePipelineTest, EnforcesCanonicalTimestampCapacityAndRejectsIngressRole) {
    const std::string three_log = unique_log_path("three_transport_timestamps");
    std::error_code three_removed;
    fs::remove(three_log, three_removed);
    Runtime three(options(
        "three_transport_timestamps", "sync", "copy", 1, 1, 1, three_log, std::chrono::seconds(1),
        -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 3));
    ASSERT_TRUE(three.Configure().has_value());
    ASSERT_TRUE(three.Start().has_value());
    ASSERT_TRUE(wait_for_records(three, three_log, 1));
    ASSERT_TRUE(three.Stop().has_value());
    EXPECT_TRUE(log_contains(three_log, "timestamp_count=4 ingress_present=1 sample_present=1"));

    const std::string four_log = unique_log_path("four_transport_timestamps");
    std::error_code four_removed;
    fs::remove(four_log, four_removed);
    Runtime four(options(
        "four_transport_timestamps", "sync", "copy", 1, 1, 1, four_log,
        std::chrono::milliseconds(100), -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "",
        "", false, "", 1000, 128, 128LL * 1024LL * 1024LL, "", "", false, -1, "", "", 4));
    ASSERT_TRUE(four.Configure().has_value());
    EXPECT_FALSE(four.Start().has_value());
    auto four_failure = four.PrimaryFailure();
    ASSERT_TRUE(four_failure.has_value());
    EXPECT_EQ(four_failure->code(), ErrorCode::InvalidArgument);

    const std::string ingress_log = unique_log_path("plugin_ingress_role");
    std::error_code ingress_removed;
    fs::remove(ingress_log, ingress_removed);
    Runtime ingress(options(
        "plugin_ingress_role", "sync", "copy", 1, 1, 1, ingress_log, std::chrono::milliseconds(100),
        -1, 16, 64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 128,
        128LL * 1024LL * 1024LL, "", "", false, -1, "", "ingress_role", 1));
    ASSERT_TRUE(ingress.Configure().has_value());
    EXPECT_FALSE(ingress.Start().has_value());
    auto ingress_failure = ingress.PrimaryFailure();
    ASSERT_TRUE(ingress_failure.has_value());
    EXPECT_EQ(ingress_failure->code(), ErrorCode::InvalidArgument);
}

TEST(RuntimePipelineTest, ExposesStableActivationClockCorrelation) {
    const std::string log = unique_log_path("clock_correlation_probe");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "clock_correlation_probe", "sync", "copy", 1, 1, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "clock_probe"));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_TRUE(log_contains(log, "storage_clock_probe ingress="));
}

TEST(RuntimePipelineTest, RefreshesIngressClockAcrossRestart) {
    const std::string log = unique_log_path("clock_restart");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "clock_restart", "sync", "copy", 1, 1, 1, log, std::chrono::seconds(1), -1, 16, 64U * 1024U,
        8, AdmissionMode::Wait, 0, "clock_probe"));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.Stop().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 2));
    ASSERT_TRUE(runtime.Stop().has_value());

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
        ASSERT_TRUE(runtime.Configure().has_value());
        const auto started = Clock::now();
        EXPECT_FALSE(runtime.Start().has_value());
        EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(500));
        EXPECT_EQ(runtime.State(), RuntimeState::Failed);
        auto primary = runtime.PrimaryFailure();
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
        {"resume_old", false, 5, "disposition=2"},
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
        ASSERT_TRUE(runtime.Configure().has_value());
        ASSERT_TRUE(runtime.Start().has_value());
        ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 5));
        ASSERT_TRUE(runtime.Stop().has_value());
        EXPECT_EQ(runtime.State(), RuntimeState::Stopped);
        EXPECT_EQ(record_count(log), scenario.expected_records);
        EXPECT_TRUE(log_contains(log, scenario.expected_abort));
    }

    const std::string log = unique_log_path("commit_failure");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "commit_failure", "sync", "copy", 1, 6, 1, log, std::chrono::milliseconds(100), 2, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "commit_failure_switch"));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_failure(runtime));
    EXPECT_FALSE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_failure(runtime));
    auto primary = runtime.PrimaryFailure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Timeout);
    EXPECT_FALSE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
}

TEST(RuntimePipelineTest, RejectsCanonicalRegistrationCapacityExhaustion) {
    const std::string log = unique_log_path("registration_capacity");
    std::error_code removed;
    fs::remove(log, removed);
    auto runtime_options = options("registration_capacity", "sync", "copy", 2, 1, 1, log);
    runtime_options.max_streams = 1;
    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.Configure().has_value());
    EXPECT_FALSE(runtime.Start().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
}

TEST(RuntimePipelineTest, EnforcesNegotiatedStorageInflightCapacity) {
    const std::string log = unique_log_path("storage_capacity");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "storage_capacity", "sync", "copy", 1, 8, 1, log, std::chrono::seconds(1), -1, 16,
        64U * 1024U, 8, AdmissionMode::Wait, 0, "ready", "", "", false, "", 1000, 2, 32));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 8));
    ASSERT_TRUE(runtime.Stop().has_value());
    const auto snapshot = runtime.Stats();
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
    ASSERT_TRUE(invalid.Configure().has_value());
    EXPECT_FALSE(invalid.Start().has_value());
    EXPECT_EQ(invalid.State(), RuntimeState::Failed);
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
        ASSERT_TRUE(runtime.Configure().has_value());
        ASSERT_TRUE(runtime.Start().has_value());
        ASSERT_TRUE(wait_for_records(runtime, log, static_cast<std::size_t>(scenario.records)));
        ASSERT_TRUE(runtime.Stop().has_value());
        const auto snapshot = runtime.Stats();
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 4));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Stopped);
}

TEST(RuntimePipelineTest, HandlesZeroLengthAndLargePayloads) {
    const std::string zero_log = unique_log_path("zero_payload");
    std::error_code zero_removed;
    fs::remove(zero_log, zero_removed);
    Runtime zero_runtime(options(
        "zero_payload", "sync", "copy", 1, 1, 1, zero_log, std::chrono::milliseconds(500), -1, 0));
    ASSERT_TRUE(zero_runtime.Configure().has_value());
    ASSERT_TRUE(zero_runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(zero_runtime, zero_log, 1));
    ASSERT_TRUE(zero_runtime.Stop().has_value());

    const std::string log = unique_log_path("large_payload");
    std::error_code removed;
    fs::remove(log, removed);
    constexpr std::int64_t kLargeBytes = 2700 * 1024;
    Runtime runtime(options(
        "large_payload", "sync", "copy", 1, 1, 1, log, std::chrono::milliseconds(500), -1,
        kLargeBytes, 3U * 1024U * 1024U));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.Stop().has_value());
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Stopped);
}

TEST(RuntimePipelineTest, DetectsSameStackCompletionAndFailsClosed) {
    const auto log = unique_log_path("same_stack");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(
        options("same_stack", "same_stack", "copy", 1, 1, 1, log, std::chrono::milliseconds(100)));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (runtime.State() != RuntimeState::Failed && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
    EXPECT_FALSE(runtime.Stop().has_value());
    EXPECT_TRUE(runtime.PrimaryFailure().has_value());
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
    ASSERT_TRUE(wait_runtime.Configure().has_value());
    ASSERT_TRUE(wait_runtime.Start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(wait_runtime, wait_log, "submit ", 8));
    ASSERT_TRUE(wait_runtime.Stop().has_value());
    const auto wait_stats = wait_runtime.Stats();
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
    ASSERT_TRUE(reject_runtime.Configure().has_value());
    ASSERT_TRUE(reject_runtime.Start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(reject_runtime, reject_log, "submit ", 8));
    ASSERT_TRUE(reject_runtime.Stop().has_value());
    const auto reject_stats = reject_runtime.Stats();
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 4));
    ASSERT_TRUE(runtime.Stop().has_value());
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", 2));
    ASSERT_TRUE(runtime.Stop().has_value());
    const auto snapshot = runtime.Stats();
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
        ASSERT_TRUE(runtime.Configure().has_value());
        const auto start_result = runtime.Start();
        ASSERT_TRUE(start_result.has_value())
            << (start_result.has_value() ? std::string_view{} : start_result.error().message());
        ASSERT_TRUE(wait_for_records(runtime, log, 1));

        const auto started = Clock::now();
        EXPECT_FALSE(runtime.Stop().has_value());
        const auto elapsed = Clock::now() - started;
        EXPECT_LT(elapsed, std::chrono::milliseconds(500));
        EXPECT_EQ(runtime.State(), RuntimeState::Failed);
        auto primary = runtime.PrimaryFailure();
        ASSERT_TRUE(primary.has_value());
        EXPECT_EQ(primary->code(), ErrorCode::Timeout);
        EXPECT_FALSE(runtime.Stop().has_value());
        EXPECT_EQ(runtime.State(), RuntimeState::Failed);
    }
}

TEST(RuntimePipelineTest, DetectsStorageHandoffStallWithoutCallingStop) {
    const std::string log = unique_log_path("handoff_stall_watchdog");
    std::error_code removed;
    fs::remove(log, removed);
    Runtime runtime(options(
        "handoff_stall_watchdog", "storage_stall", "copy", 1, 1, 1, log,
        std::chrono::milliseconds(50)));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(wait_for_failure(runtime));
    auto primary = runtime.PrimaryFailure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::Timeout);
    const auto started = Clock::now();
    EXPECT_FALSE(runtime.Stop().has_value());
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(500));
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
        ASSERT_TRUE(runtime.Configure().has_value());
        ASSERT_TRUE(runtime.Start().has_value());
        ASSERT_TRUE(wait_for_records(runtime, log, 1));

        const auto started = Clock::now();
        EXPECT_FALSE(runtime.Stop().has_value());
        const auto elapsed = Clock::now() - started;
        EXPECT_LT(elapsed, std::chrono::milliseconds(500));
        EXPECT_EQ(runtime.State(), RuntimeState::Failed);
        auto primary = runtime.PrimaryFailure();
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
        RuntimePluginOptions acquisition;
        acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
        acquisition.config = map_config({
            {"streams", plugin::ConfigTree::Int64(1)},
            {"records_per_stream", plugin::ConfigTree::Int64(1)},
            {"producer_threads", plugin::ConfigTree::Int64(1)},
            {"payload_size", plugin::ConfigTree::Int64(16)},
            {"payload_mode", plugin::ConfigTree::String("copy")},
            {"descriptor_change_after", plugin::ConfigTree::Int64(-1)},
            {"binding_scenario", plugin::ConfigTree::String("")},
            {"lifecycle_scenario", plugin::ConfigTree::String(lifecycle_scenario)},
            {"pause_before_switch", plugin::ConfigTree::Int64(0)},
            {"stale_handle_file", plugin::ConfigTree::String("")},
            {"projection_id", plugin::ConfigTree::String("")},
            {"projection_before_start", plugin::ConfigTree::Int64(0)},
            {"projection_delay_after", plugin::ConfigTree::Int64(-1)},
            {"projection_registration_mode", plugin::ConfigTree::String("")},
            {"timestamp_scenario", plugin::ConfigTree::String("")},
            {"timestamp_key_count", plugin::ConfigTree::Int64(1)},
            {"event_log", plugin::ConfigTree::String(log)},
        });
        runtime_options.acquisitions.push_back(std::move(acquisition));
    }

    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.Configure().has_value());
    EXPECT_FALSE(runtime.Start().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
    auto primary = runtime.PrimaryFailure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_NE(primary->message().find("partial start failure"), std::string_view::npos);
    auto secondary = runtime.SecondaryFailures();
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
    ASSERT_TRUE(runtime.Configure().has_value());
    EXPECT_FALSE(runtime.Start().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);
    auto primary = runtime.PrimaryFailure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_NE(primary->message().find("partial start failure"), std::string_view::npos);
    auto secondary = runtime.SecondaryFailures();
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
    ASSERT_TRUE(runtime.Configure().has_value());
    const auto start_result = runtime.Start();
    ASSERT_TRUE(start_result.has_value())
        << (start_result.has_value() ? std::string_view{} : start_result.error().message());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    EXPECT_FALSE(runtime.Stop().has_value());
    EXPECT_EQ(runtime.State(), RuntimeState::Failed);

    auto primary = runtime.PrimaryFailure();
    ASSERT_TRUE(primary.has_value());
    EXPECT_EQ(primary->code(), ErrorCode::StorageError);
    EXPECT_NE(primary->message().find("flush failure"), std::string_view::npos);
    auto secondary = runtime.SecondaryFailures();
    ASSERT_TRUE(secondary.has_value());
    ASSERT_EQ(secondary->size(), 1U);
    EXPECT_EQ(secondary->front().code(), ErrorCode::StorageError);
    EXPECT_NE(secondary->front().message().find("close failure"), std::string_view::npos);
}

TEST(RuntimePipelineTest, FailsClosedOnStorageSubmitAndBackgroundFailures) {
    const std::string submit_log = unique_log_path("submit_error");
    std::error_code submit_removed;
    fs::remove(submit_log, submit_removed);
    Runtime submit_runtime(options("submit_error", "submit_error", "copy", 1, 1, 1, submit_log));
    ASSERT_TRUE(submit_runtime.Configure().has_value());
    ASSERT_TRUE(submit_runtime.Start().has_value());
    ASSERT_TRUE(wait_for_failure(submit_runtime));
    EXPECT_FALSE(submit_runtime.Stop().has_value());
    EXPECT_EQ(submit_runtime.State(), RuntimeState::Failed);

    const std::string background_log = unique_log_path("background_fatal");
    std::error_code background_removed;
    fs::remove(background_log, background_removed);
    Runtime background_runtime(
        options("background_fatal", "background_fatal", "copy", 1, 1, 1, background_log));
    ASSERT_TRUE(background_runtime.Configure().has_value());
    ASSERT_TRUE(background_runtime.Start().has_value());
    ASSERT_TRUE(wait_for_failure(background_runtime));
    EXPECT_FALSE(background_runtime.Stop().has_value());
    auto primary = background_runtime.PrimaryFailure();
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_TRUE(fs::is_regular_file(handle_file));

    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 2));
    ASSERT_TRUE(runtime.Stop().has_value());
    EXPECT_TRUE(log_contains(log, "stale_handles_rejected"));
}

TEST(RuntimePipelineTest, QualifiesMixedPayloadModesInOneActivation) {
    const std::string log = unique_log_path("mixed_payload_modes");
    std::error_code removed;
    fs::remove(log, removed);
    auto runtime_options = options("mixed_payload_modes", "sync", "copy", 1, 2, 1, log);
    runtime_options.acquisitions.clear();
    for (const std::string payload_mode : {"copy", "adopt", "retain"}) {
        RuntimePluginOptions acquisition;
        acquisition.path = required_path("RODAQ_FAKE_ACQUISITION_PLUGIN");
        acquisition.config = map_config({
            {"streams", plugin::ConfigTree::Int64(1)},
            {"records_per_stream", plugin::ConfigTree::Int64(2)},
            {"producer_threads", plugin::ConfigTree::Int64(1)},
            {"payload_size", plugin::ConfigTree::Int64(16)},
            {"payload_mode", plugin::ConfigTree::String(payload_mode)},
            {"descriptor_change_after", plugin::ConfigTree::Int64(-1)},
            {"binding_scenario", plugin::ConfigTree::String("")},
            {"lifecycle_scenario", plugin::ConfigTree::String("")},
            {"pause_before_switch", plugin::ConfigTree::Int64(0)},
            {"stale_handle_file", plugin::ConfigTree::String("")},
            {"event_log", plugin::ConfigTree::String(log)},
        });
        runtime_options.acquisitions.push_back(std::move(acquisition));
    }

    Runtime runtime(std::move(runtime_options));
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 6));
    ASSERT_TRUE(runtime.Stop().has_value());
    const auto snapshot = runtime.Stats();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->accepted_records, 6U);
    EXPECT_EQ(snapshot->accepted_bytes, 96U);
    EXPECT_EQ(snapshot->inflight_records, 0U);
    EXPECT_EQ(record_count(log), 6U);
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_log_prefix(runtime, log, "submit ", kSubmissions));
    ASSERT_TRUE(runtime.Stop().has_value());

    const auto snapshot = runtime.Stats();
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
    ASSERT_TRUE(runtime.Configure().has_value());
    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 1));
    ASSERT_TRUE(runtime.Stop().has_value());

    ASSERT_TRUE(runtime.Start().has_value());
    ASSERT_TRUE(wait_for_records(runtime, log, 2));
    ASSERT_TRUE(runtime.Stop().has_value());
    const auto lines = read_lines(log);
    EXPECT_EQ(
        std::count_if(
            lines.begin(), lines.end(),
            [](const std::string& line) { return line.rfind("record ", 0) == 0; }),
        2);
}

}  // namespace
}  // namespace rodaq::runtime
