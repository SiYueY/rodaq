#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/result.hpp>

namespace rodaq {

using PipelineId = std::uint64_t;
using StreamId = std::uint64_t;
using ControlGeneration = std::uint64_t;

enum class RecordingMode { Continuous, Rolling };
enum class StartPolicyMode { AllRequired, BestEffort };
enum class RecoveryPolicyMode { Manual, Resume };
enum class ReadinessPolicy { EntryReady, SourceReady };
enum class RetentionMode { KeepAll, Rolling };

struct SegmentPolicy {
    std::optional<std::chrono::nanoseconds> max_duration;
    std::optional<std::uint64_t> max_physical_bytes;
};

struct RetentionPolicy {
    RetentionMode mode{RetentionMode::KeepAll};
    std::optional<std::chrono::nanoseconds> max_duration;
    std::optional<std::uint64_t> max_physical_bytes;
};

struct PluginConfig {
    std::string plugin_id;
    std::string plugin_version;
    std::string json{"{}"};
};

struct StreamConfig {
    StreamId stream_id{0};  // Zero requests allocation during Create/Configure.
    std::string stream_key;
    std::string name;  // Empty represents the optional field's absence.
    ReadinessPolicy readiness_policy{ReadinessPolicy::EntryReady};
};

struct PipelineConfig {
    PipelineId pipeline_id{0};
    bool required{true};
    std::string resource_profile;
    std::string output_root;
    PluginConfig acquisition;
    PluginConfig storage;
    std::vector<StreamConfig> streams;
};

struct TaskConfig {
    std::string name;
    RecordingMode recording_mode{RecordingMode::Continuous};
    StartPolicyMode start_policy{StartPolicyMode::AllRequired};
    RecoveryPolicyMode recovery_policy{RecoveryPolicyMode::Manual};
    SegmentPolicy segment_policy;
    RetentionPolicy retention_policy;
    std::vector<PipelineConfig> pipelines;
};

// Parsing limits are resource bounds, not permission or installed-Plugin facts.
// TaskService revalidates against its Deployment before durable acceptance.
struct TaskConfigLimits {
    std::size_t max_config_bytes{1024 * 1024};
    std::size_t max_plugin_config_bytes{65536};
    std::size_t max_pipelines{8};
    std::size_t max_streams_per_pipeline{64};
    std::size_t max_json_depth{32};
    std::size_t max_json_string_bytes{65536};
    std::size_t max_json_object_members{256};
    std::size_t max_json_array_elements{4096};
    std::size_t max_json_nodes{16384};
};

Result<TaskConfig> parse_task_config(
    std::string_view portable_json, const TaskConfigLimits& limits = {});
Result<std::string> export_task_config(
    const TaskConfig& config, const TaskConfigLimits& limits = {});

}  // namespace rodaq
