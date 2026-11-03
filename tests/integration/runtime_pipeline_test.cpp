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
    std::size_t block_bytes = 64U * 1024U) {
    RuntimeOptions value;
    value.target_uri = "file://fake";
    value.session_id = std::string(name);
    value.payload_block_bytes = block_bytes;
    value.payload_block_count = 16;
    value.max_single_payload_bytes = block_bytes;
    value.dispatch_capacity = 8;
    value.operation_capacity = 8;
    value.completion_capacity = 8;
    value.submit_wait_timeout = std::chrono::milliseconds(500);
    value.stop_progress_timeout = stop_timeout;
    value.storage.path = required_path("RODAQ_FAKE_STORAGE_PLUGIN");
    value.storage.config = map_config({
        {"scenario", plugin::ConfigTree::String(std::move(storage_scenario))},
        {"event_log", plugin::ConfigTree::String(event_log)},
        {"callback_delay_us", plugin::ConfigTree::Int64(1000)},
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

std::size_t record_count(const std::string& path) {
    const auto lines = read_lines(path);
    return static_cast<std::size_t>(std::count_if(
        lines.begin(), lines.end(),
        [](const std::string& line) { return line.rfind("record ", 0) == 0; }));
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

TEST(RuntimePipelineTest, MissingCallbacksDoNotHangStop) {
    expect_failed_scenario("missing_callback", "missing_callback", std::chrono::milliseconds(100));
}

TEST(RuntimePipelineTest, CallbackAfterCloseFailsClosed) {
    expect_failed_scenario(
        "callback_after_close", "callback_after_close", std::chrono::milliseconds(100));
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
