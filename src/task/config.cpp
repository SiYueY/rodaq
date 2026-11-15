#include "config.hpp"

#include "../persistence/decimal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/version.hpp>

namespace rodaq::task {

persistence::JsonLimits config_json_limits(const TaskConfigLimits& limits) {
    return {limits.max_config_bytes,        limits.max_json_depth,
            limits.max_json_string_bytes,   limits.max_json_object_members,
            limits.max_json_array_elements, limits.max_json_nodes};
}

namespace {

using persistence::Json;

struct InvalidConfig {
    StatusCode code{StatusCode::InvalidArgument};
};

void require(bool predicate) {
    if (!predicate) {
        throw InvalidConfig{};
    }
}

bool visible(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return byte >= 0x21 && byte <= 0x7e;
           });
}

bool label(std::string_view value) {
    return value.size() <= 128 && plugin::abi::utf8_prefix(value, value.size()) == value.size();
}

void fields(
    const Json& object, std::initializer_list<const char*> required,
    std::initializer_list<const char*> optional = {}) {
    require(
        object.is_object() && object.size() >= required.size() &&
        object.size() <= required.size() + optional.size());
    for (const auto* name : required) {
        require(object.contains(name));
    }
    for (auto field = object.begin(); field != object.end(); ++field) {
        const auto named = [&](const char* name) { return field.key() == name; };
        require(
            std::any_of(required.begin(), required.end(), named) ||
            std::any_of(optional.begin(), optional.end(), named));
    }
}

std::string_view text(const Json& value) {
    require(value.is_string());
    return value.get_ref<const std::string&>();
}

std::uint64_t unsigned_text(const Json& value) {
    require(value.is_string());
    const auto parsed = persistence::unsigned_decimal(value.get_ref<const std::string&>());
    require(parsed.ok());
    return parsed.value();
}

std::optional<std::uint64_t> bytes_bound(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    const auto bytes = unsigned_text(value);
    require(bytes != 0);
    return bytes;
}

std::optional<std::chrono::nanoseconds> duration_bound(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    fields(value, {"value", "unit"});
    const auto count = unsigned_text(value.at("value"));
    const auto unit = text(value.at("unit"));
    const std::uint64_t multiplier = unit == "ns"   ? 1
                                     : unit == "us" ? 1000
                                     : unit == "ms" ? 1000000
                                     : unit == "s"  ? 1000000000
                                                    : 0;
    require(
        multiplier != 0 && count != 0 &&
        count <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / multiplier);
    return std::chrono::nanoseconds(static_cast<std::int64_t>(count * multiplier));
}

PluginConfig plugin_config(const Json& value, const TaskConfigLimits& limits) {
    fields(value, {"plugin_id", "plugin_version", "config"});
    require(value.at("config").is_object());
    auto canonical = persistence::canonical_json(value.at("config"), config_json_limits(limits));
    if (!canonical.ok()) {
        throw InvalidConfig{canonical.status().code()};
    }
    require(canonical.value().size() <= limits.max_plugin_config_bytes);
    const auto id = text(value.at("plugin_id"));
    const auto version = text(value.at("plugin_version"));
    require(plugin::abi::valid_identifier(id));
    auto valid = plugin::validate_plugin_version(version);
    if (!valid.ok()) {
        throw InvalidConfig{valid.status().code()};
    }
    return {std::string(id), std::string(version), std::move(canonical).value()};
}

TaskConfig from_document(const Json& value, const TaskConfigLimits& limits) {
    fields(
        value,
        {"format_version", "recording_mode", "start_policy", "recovery_policy", "segment_policy",
         "retention_policy", "pipelines"},
        {"name"});
    require(text(value.at("format_version")) == "1.0");
    TaskConfig config;
    if (value.contains("name")) {
        const auto name = text(value.at("name"));
        require(!name.empty() && label(name));
        config.name = name;
    }
    const auto mode = text(value.at("recording_mode"));
    require(mode == "continuous" || mode == "rolling");
    config.recording_mode =
        mode == "continuous" ? RecordingMode::Continuous : RecordingMode::Rolling;
    fields(value.at("start_policy"), {"mode"});
    const auto start = text(value.at("start_policy").at("mode"));
    require(start == "all_required" || start == "best_effort");
    config.start_policy =
        start == "all_required" ? StartPolicyMode::AllRequired : StartPolicyMode::BestEffort;
    fields(value.at("recovery_policy"), {"mode"});
    const auto recovery = text(value.at("recovery_policy").at("mode"));
    require(recovery == "manual" || recovery == "resume");
    config.recovery_policy =
        recovery == "manual" ? RecoveryPolicyMode::Manual : RecoveryPolicyMode::Resume;
    const auto& segment = value.at("segment_policy");
    fields(segment, {"max_duration", "max_physical_bytes"});
    config.segment_policy = {
        duration_bound(segment.at("max_duration")), bytes_bound(segment.at("max_physical_bytes"))};
    const auto& retention = value.at("retention_policy");
    fields(retention, {"mode", "max_duration", "max_physical_bytes"});
    const auto retain_mode = text(retention.at("mode"));
    require(retain_mode == "keep_all" || retain_mode == "rolling");
    config.retention_policy = {
        retain_mode == "keep_all" ? RetentionMode::KeepAll : RetentionMode::Rolling,
        duration_bound(retention.at("max_duration")),
        bytes_bound(retention.at("max_physical_bytes"))};
    const auto& pipelines = value.at("pipelines");
    require(pipelines.is_array() && !pipelines.empty() && pipelines.size() <= limits.max_pipelines);
    config.pipelines.reserve(pipelines.size());
    for (const auto& item : pipelines) {
        fields(
            item,
            {"required", "resource_profile", "output_root", "acquisition", "storage", "streams"},
            {"pipeline_id"});
        PipelineConfig pipeline;
        if (item.contains("pipeline_id")) {
            pipeline.pipeline_id = unsigned_text(item.at("pipeline_id"));
            require(pipeline.pipeline_id != 0);
        }
        require(item.at("required").is_boolean());
        pipeline.required = item.at("required").get<bool>();
        require(
            visible(text(item.at("resource_profile")), 128) &&
            visible(text(item.at("output_root")), 128));
        pipeline.resource_profile = text(item.at("resource_profile"));
        pipeline.output_root = text(item.at("output_root"));
        pipeline.acquisition = plugin_config(item.at("acquisition"), limits);
        pipeline.storage = plugin_config(item.at("storage"), limits);
        const auto& streams = item.at("streams");
        require(
            streams.is_array() && !streams.empty() &&
            streams.size() <= limits.max_streams_per_pipeline);
        pipeline.streams.reserve(streams.size());
        for (const auto& entry : streams) {
            fields(entry, {"stream_key", "readiness_policy"}, {"stream_id", "name"});
            StreamConfig stream;
            if (entry.contains("stream_id")) {
                stream.stream_id = unsigned_text(entry.at("stream_id"));
                require(stream.stream_id != 0);
            }
            require(visible(text(entry.at("stream_key")), 128));
            stream.stream_key = text(entry.at("stream_key"));
            if (entry.contains("name")) {
                const auto name = text(entry.at("name"));
                require(!name.empty() && label(name));
                stream.name = name;
            }
            const auto readiness = text(entry.at("readiness_policy"));
            require(readiness == "entry_ready" || readiness == "source_ready");
            stream.readiness_policy = readiness == "entry_ready" ? ReadinessPolicy::EntryReady
                                                                 : ReadinessPolicy::SourceReady;
            pipeline.streams.push_back(std::move(stream));
        }
        config.pipelines.push_back(std::move(pipeline));
    }
    return config;
}

Json duration(const std::optional<std::chrono::nanoseconds>& value) {
    if (!value) {
        return nullptr;
    }
    return Json{{"value", std::to_string(value->count())}, {"unit", "ns"}};
}

Json bytes(const std::optional<std::uint64_t>& value) {
    return value ? Json(std::to_string(*value)) : Json(nullptr);
}

bool consume_nodes(const Json& value, std::size_t& remaining) {
    if (remaining == 0) {
        return false;
    }
    --remaining;
    if (value.is_structured()) {
        for (const auto& child : value) {
            if (!consume_nodes(child, remaining)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

Result<TaskConfig> normalize_config(const TaskConfig& config, const TaskConfigLimits& limits) {
    if (config.pipelines.empty() || config.pipelines.size() > limits.max_pipelines ||
        !label(config.name) || limits.max_plugin_config_bytes > limits.max_config_bytes) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        require(
            config.recording_mode == RecordingMode::Continuous ||
            config.recording_mode == RecordingMode::Rolling);
        require(
            config.start_policy == StartPolicyMode::AllRequired ||
            config.start_policy == StartPolicyMode::BestEffort);
        require(
            config.recovery_policy == RecoveryPolicyMode::Manual ||
            config.recovery_policy == RecoveryPolicyMode::Resume);

        const auto valid_duration = [](const auto& value) { return !value || value->count() > 0; };
        const auto valid_bytes = [](const auto& value) { return !value || *value > 0; };
        require(
            valid_duration(config.segment_policy.max_duration) &&
            valid_bytes(config.segment_policy.max_physical_bytes) &&
            (config.segment_policy.max_duration || config.segment_policy.max_physical_bytes));
        require(
            valid_duration(config.retention_policy.max_duration) &&
            valid_bytes(config.retention_policy.max_physical_bytes));
        if (config.recording_mode == RecordingMode::Continuous) {
            require(
                config.retention_policy.mode == RetentionMode::KeepAll &&
                !config.retention_policy.max_duration &&
                !config.retention_policy.max_physical_bytes);
        } else {
            require(
                config.retention_policy.mode == RetentionMode::Rolling &&
                (config.retention_policy.max_duration ||
                 config.retention_policy.max_physical_bytes));
        }

        std::size_t input_bytes = 0;
        std::size_t model_nodes = 1;
        const auto charge = [&](std::string_view value) {
            require(value.size() <= limits.max_config_bytes - input_bytes);
            input_bytes += value.size();
        };
        charge(config.name);
        for (const auto& pipeline : config.pipelines) {
            require(model_nodes < limits.max_json_nodes);
            ++model_nodes;
            require(
                pipeline.streams.size() <= limits.max_streams_per_pipeline &&
                pipeline.streams.size() <= limits.max_json_nodes - model_nodes);
            model_nodes += pipeline.streams.size();
            charge(pipeline.resource_profile);
            charge(pipeline.output_root);
            for (const auto* backend : {&pipeline.acquisition, &pipeline.storage}) {
                charge(backend->plugin_id);
                charge(backend->plugin_version);
                charge(backend->json);
            }
            for (const auto& stream : pipeline.streams) {
                charge(stream.stream_key);
                charge(stream.name);
            }
        }

        std::set<PipelineId> pipelines;
        std::set<StreamId> streams;
        for (const auto& pipeline : config.pipelines) {
            require(pipeline.pipeline_id == 0 || pipelines.insert(pipeline.pipeline_id).second);
            require(visible(pipeline.resource_profile, 128) && visible(pipeline.output_root, 128));
            require(
                !pipeline.streams.empty() &&
                pipeline.streams.size() <= limits.max_streams_per_pipeline);
            for (const auto* backend : {&pipeline.acquisition, &pipeline.storage}) {
                require(
                    plugin::abi::valid_identifier(backend->plugin_id) &&
                    backend->json.size() <= limits.max_plugin_config_bytes);
                auto version = plugin::validate_plugin_version(backend->plugin_version);
                if (!version.ok()) {
                    return std::move(version).status();
                }
            }

            std::set<std::string_view> keys;
            for (const auto& stream : pipeline.streams) {
                require(stream.stream_id == 0 || streams.insert(stream.stream_id).second);
                require(
                    visible(stream.stream_key, 128) && label(stream.name) &&
                    keys.insert(stream.stream_key).second);
                require(
                    stream.readiness_policy == ReadinessPolicy::EntryReady ||
                    stream.readiness_policy == ReadinessPolicy::SourceReady);
            }
        }

        TaskConfig normalized = config;
        for (auto& pipeline : normalized.pipelines) {
            for (auto* backend : {&pipeline.acquisition, &pipeline.storage}) {
                auto parsed = persistence::parse_json(backend->json, config_json_limits(limits));
                if (!parsed.ok()) {
                    return std::move(parsed).status();
                }
                require(parsed.value().is_object());
                auto canonical =
                    persistence::canonical_json(parsed.value(), config_json_limits(limits));
                if (!canonical.ok()) {
                    return std::move(canonical).status();
                }
                require(canonical.value().size() <= limits.max_plugin_config_bytes);
                backend->json = std::move(canonical).value();
            }
        }
        return normalized;
    } catch (const InvalidConfig&) {
        return Status(StatusCode::InvalidArgument, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<Json> config_document(const TaskConfig& config, const TaskConfigLimits& limits) {
    auto checked = normalize_config(config, limits);
    if (!checked.ok()) {
        return std::move(checked).status();
    }
    try {
        const auto& value = checked.value();
        Json root{
            {"format_version", "1.0"},
            {"recording_mode",
             value.recording_mode == RecordingMode::Continuous ? "continuous" : "rolling"},
            {"start_policy",
             {{"mode", value.start_policy == StartPolicyMode::AllRequired ? "all_required"
                                                                          : "best_effort"}}},
            {"recovery_policy",
             {{"mode", value.recovery_policy == RecoveryPolicyMode::Manual ? "manual" : "resume"}}},
            {"segment_policy",
             {{"max_duration", duration(value.segment_policy.max_duration)},
              {"max_physical_bytes", bytes(value.segment_policy.max_physical_bytes)}}},
            {"retention_policy",
             {{"mode",
               value.retention_policy.mode == RetentionMode::KeepAll ? "keep_all" : "rolling"},
              {"max_duration", duration(value.retention_policy.max_duration)},
              {"max_physical_bytes", bytes(value.retention_policy.max_physical_bytes)}}},
            {"pipelines", Json::array()}};
        if (!value.name.empty()) {
            root["name"] = value.name;
        }
        auto remaining_nodes = limits.max_json_nodes;
        require(consume_nodes(root, remaining_nodes));
        for (const auto& item : value.pipelines) {
            Json pipeline{
                {"required", item.required},
                {"resource_profile", item.resource_profile},
                {"output_root", item.output_root},
                {"streams", Json::array()}};
            if (item.pipeline_id != 0) {
                pipeline["pipeline_id"] = std::to_string(item.pipeline_id);
            }
            require(consume_nodes(pipeline, remaining_nodes));
            for (const auto& named :
                 {std::pair{"acquisition", &item.acquisition},
                  std::pair{"storage", &item.storage}}) {
                Json backend{
                    {"plugin_id", named.second->plugin_id},
                    {"plugin_version", named.second->plugin_version}};
                require(consume_nodes(backend, remaining_nodes));
                auto bounded = config_json_limits(limits);
                bounded.max_nodes = remaining_nodes;
                auto parsed = persistence::parse_json(named.second->json, bounded);
                if (!parsed.ok()) {
                    return std::move(parsed).status();
                }
                require(consume_nodes(parsed.value(), remaining_nodes));
                backend["config"] = std::move(parsed).value();
                pipeline[named.first] = std::move(backend);
            }
            for (const auto& stream : item.streams) {
                Json entry{
                    {"stream_key", stream.stream_key},
                    {"readiness_policy", stream.readiness_policy == ReadinessPolicy::EntryReady
                                             ? "entry_ready"
                                             : "source_ready"}};
                if (stream.stream_id != 0) {
                    entry["stream_id"] = std::to_string(stream.stream_id);
                }
                if (!stream.name.empty()) {
                    entry["name"] = stream.name;
                }
                require(consume_nodes(entry, remaining_nodes));
                pipeline["streams"].push_back(std::move(entry));
            }
            root["pipelines"].push_back(std::move(pipeline));
        }
        return root;
    } catch (const InvalidConfig& failure) {
        return Status(failure.code, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> config_version(const TaskConfig& config, const TaskConfigLimits& limits) {
    for (const auto& pipeline : config.pipelines) {
        if (pipeline.pipeline_id == 0) {
            return Status(StatusCode::InvalidArgument, {});
        }
        for (const auto& stream : pipeline.streams) {
            if (stream.stream_id == 0) {
                return Status(StatusCode::InvalidArgument, {});
            }
        }
    }
    auto document = config_document(config, limits);
    if (!document.ok()) {
        return std::move(document).status();
    }
    return persistence::semantic_digest(
        document.value(), "rodaq.task-config.v1", config_json_limits(limits));
}

}  // namespace rodaq::task

namespace rodaq {

Result<TaskConfig> parse_task_config(std::string_view input, const TaskConfigLimits& limits) {
    auto parsed = persistence::parse_json(input, task::config_json_limits(limits));
    if (!parsed.ok()) {
        return std::move(parsed).status();
    }
    try {
        auto config = task::from_document(parsed.value(), limits);
        return task::normalize_config(config, limits);
    } catch (const task::InvalidConfig& error) {
        return Status(error.code, {});
    } catch (const persistence::Json::exception&) {
        return Status(StatusCode::InvalidArgument, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> export_task_config(const TaskConfig& config, const TaskConfigLimits& limits) {
    auto document = task::config_document(config, limits);
    if (!document.ok()) {
        return std::move(document).status();
    }
    return persistence::canonical_json(document.value(), task::config_json_limits(limits));
}

}  // namespace rodaq
