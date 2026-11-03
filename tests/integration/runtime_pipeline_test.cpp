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
    std::chrono::milliseconds stop_timeout = std::chrono::milliseconds(500)) {
    RuntimeOptions value;
    value.target_uri = "file://fake";
    value.session_id = std::string(name);
    value.payload_block_bytes = 64U * 1024U;
    value.payload_block_count = 16;
    value.max_single_payload_bytes = 64U * 1024U;
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
        {"payload_size", plugin::ConfigTree::Int64(16)},
        {"payload_mode", plugin::ConfigTree::String(std::move(payload_mode))},
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
