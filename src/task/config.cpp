#include "config.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <rodaq/plugin/abi.hpp>
#include <rodaq/plugin/version.hpp>

#include "../persistence/decimal.hpp"

namespace rodaq {
namespace {

using persistence::Json;

constexpr std::uint64_t kMaxJsSafeInteger = 9007199254740991ULL;

struct InvalidConfig {
    Status status;
};

void require(bool condition, const char* message = nullptr) {
    if (!condition) {
        throw InvalidConfig{Status(StatusCode::InvalidArgument, message ? message : "")};
    }
}

template <typename Value>
Value take(Result<Value> result) {
    if (!result.ok()) {
        throw InvalidConfig{std::move(result).status()};
    }
    return std::move(result).value();
}

void fields(
    const Json& object, std::initializer_list<const char*> required,
    std::initializer_list<const char*> optional = {}) {
    require(object.is_object(), "Expected JSON object");
    std::size_t present = 0;
    for (const auto* name : optional) {
        present += object.contains(name) ? 1 : 0;
    }
    require(object.size() == required.size() + present, "Unexpected or missing JSON field");
    for (const auto* name : required) {
        require(object.contains(name), "Missing required JSON field");
    }
}

const std::string& req_string(const Json& object, const char* key) {
    const auto& value = object.at(key);
    require(value.is_string(), "Expected string field");
    return value.get_ref<const std::string&>();
}

bool valid_utf8(std::string_view value, std::size_t min_bytes, std::size_t max_bytes) {
    return value.size() >= min_bytes && value.size() <= max_bytes &&
           value.find('\0') == std::string_view::npos &&
           plugin::abi::utf8_prefix(value, value.size()) == value.size();
}

bool valid_visible_ascii(std::string_view value, std::size_t max_bytes) {
    return !value.empty() && value.size() <= max_bytes &&
           std::all_of(value.begin(), value.end(), [](unsigned char byte) {
               return byte >= 0x21 && byte <= 0x7e;
           });
}

std::optional<std::uint64_t> nullable_safe_uint(const Json& object, const char* key) {
    if (!object.contains(key) || object.at(key).is_null()) {
        return std::nullopt;
    }
    const auto& value = object.at(key);
    require(
        value.is_number_integer() || value.is_number_unsigned(),
        "Expected positive integer or null");
    if (value.is_number_integer()) {
        const auto signed_val = value.get<std::int64_t>();
        require(
            signed_val > 0 && static_cast<std::uint64_t>(signed_val) <= kMaxJsSafeInteger,
            "Integer out of safe positive range");
        return static_cast<std::uint64_t>(signed_val);
    }
    const auto unsigned_val = value.get<std::uint64_t>();
    require(
        unsigned_val > 0 && unsigned_val <= kMaxJsSafeInteger,
        "Integer out of safe positive range");
    return unsigned_val;
}

std::uint64_t optional_id_field(const Json& object, const char* key) {
    if (!object.contains(key) || object.at(key).is_null()) {
        return 0;
    }
    const auto& value = object.at(key);
    require(value.is_string(), "Identity field must be a positive decimal string");
    const auto parsed = take(persistence::unsigned_decimal(value.get_ref<const std::string&>()));
    require(parsed != 0, "Identity must be non-zero");
    return parsed;
}

void validate_limits(const TaskConfigLimits& limits) {
    require(
        limits.max_config_bytes > 0 && limits.max_config_bytes <= 16 * 1024 * 1024 &&
        limits.max_plugin_config_bytes > 0 && limits.max_plugin_config_bytes <= 4 * 1024 * 1024 &&
        limits.max_pipelines > 0 && limits.max_pipelines <= 64 &&
        limits.max_streams_per_pipeline > 0 && limits.max_streams_per_pipeline <= 1024 &&
        limits.max_json_depth >= 8 && limits.max_json_depth <= 64);
}

void validate_plugin_config(const PluginConfig& plugin, const TaskConfigLimits& limits) {
    require(plugin::abi::valid_identifier(plugin.plugin_id), "Invalid plugin_id");
    require(plugin.plugin_version.size() <= 128, "Invalid plugin_version");
    take(plugin::validate_plugin_version(plugin.plugin_version));
    require(
        !plugin.json.empty() && plugin.json.size() <= limits.max_plugin_config_bytes,
        "Plugin config size out of bounds");
    const auto parsed = take(persistence::parse_json(
        plugin.json, {limits.max_plugin_config_bytes, limits.max_json_depth}));
    require(parsed.is_object(), "Plugin config must be a JSON object");
}

void validate_config_impl(
    const TaskConfig& config, const TaskConfigLimits& limits, bool require_ids) {
    validate_limits(limits);
    if (!config.name.empty()) {
        require(valid_utf8(config.name, 1, 128), "Invalid task name");
    }
    // Segment policy: at least one bound enabled.
    require(
        config.segment_policy.max_duration.has_value() ||
            config.segment_policy.max_physical_bytes.has_value(),
        "segment_policy requires max_duration_ms or max_physical_bytes");
    if (config.segment_policy.max_duration) {
        const auto ms = config.segment_policy.max_duration->count();
        require(ms > 0 && static_cast<std::uint64_t>(ms) <= kMaxJsSafeInteger);
    }
    if (config.segment_policy.max_physical_bytes) {
        require(
            *config.segment_policy.max_physical_bytes > 0 &&
            *config.segment_policy.max_physical_bytes <= kMaxJsSafeInteger);
    }
    // Retention policy.
    if (config.retention_policy.mode == RetentionMode::KeepAll) {
        require(
            !config.retention_policy.max_duration.has_value() &&
                !config.retention_policy.max_physical_bytes.has_value(),
            "keep_all retention forbids duration/byte bounds");
    } else {
        require(config.retention_policy.mode == RetentionMode::Rolling);
        require(
            config.retention_policy.max_duration.has_value() ||
                config.retention_policy.max_physical_bytes.has_value(),
            "rolling retention requires max_duration_ms or max_physical_bytes");
        if (config.retention_policy.max_duration) {
            const auto ms = config.retention_policy.max_duration->count();
            require(ms > 0 && static_cast<std::uint64_t>(ms) <= kMaxJsSafeInteger);
        }
        if (config.retention_policy.max_physical_bytes) {
            require(
                *config.retention_policy.max_physical_bytes > 0 &&
                *config.retention_policy.max_physical_bytes <= kMaxJsSafeInteger);
        }
    }
    // Pipelines.
    require(
        !config.pipelines.empty() && config.pipelines.size() <= limits.max_pipelines,
        "Invalid pipeline count");
    std::vector<PipelineId> seen_pipelines;
    std::vector<StreamId> seen_streams;
    for (const auto& pipeline : config.pipelines) {
        if (require_ids) {
            require(pipeline.pipeline_id != 0, "Missing pipeline_id");
        }
        if (pipeline.pipeline_id != 0) {
            require(
                std::find(seen_pipelines.begin(), seen_pipelines.end(), pipeline.pipeline_id) ==
                    seen_pipelines.end(),
                "Duplicate pipeline_id");
            seen_pipelines.push_back(pipeline.pipeline_id);
        }
        require(valid_visible_ascii(pipeline.resource_profile, 128), "Invalid resource_profile");
        require(valid_visible_ascii(pipeline.output_root, 128), "Invalid output_root");
        validate_plugin_config(pipeline.acquisition, limits);
        validate_plugin_config(pipeline.storage, limits);
        require(
            !pipeline.streams.empty() && pipeline.streams.size() <= limits.max_streams_per_pipeline,
            "Invalid stream count");
        std::vector<std::string_view> pipeline_stream_keys;
        for (const auto& stream : pipeline.streams) {
            if (require_ids) {
                require(stream.stream_id != 0, "Missing stream_id");
            }
            if (stream.stream_id != 0) {
                require(
                    std::find(seen_streams.begin(), seen_streams.end(), stream.stream_id) ==
                        seen_streams.end(),
                    "Duplicate stream_id");
                seen_streams.push_back(stream.stream_id);
            }
            require(valid_visible_ascii(stream.stream_key, 128), "Invalid stream_key");
            require(
                std::find(
                    pipeline_stream_keys.begin(), pipeline_stream_keys.end(), stream.stream_key) ==
                    pipeline_stream_keys.end(),
                "Duplicate stream_key in pipeline");
            pipeline_stream_keys.push_back(stream.stream_key);
            if (!stream.name.empty()) {
                require(valid_utf8(stream.name, 1, 128), "Invalid stream name");
            }
            require(
                stream.readiness_policy == ReadinessPolicy::EntryReady ||
                stream.readiness_policy == ReadinessPolicy::SourceReady);
        }
    }
}

Json to_json(const TaskConfig& config, const TaskConfigLimits& limits) {
    Json segment_policy{
        {"max_duration_ms", config.segment_policy.max_duration
                                ? Json(config.segment_policy.max_duration->count())
                                : Json(nullptr)},
        {"max_physical_bytes", config.segment_policy.max_physical_bytes
                                   ? Json(*config.segment_policy.max_physical_bytes)
                                   : Json(nullptr)}};
    Json retention_policy{
        {"mode", config.retention_policy.mode == RetentionMode::KeepAll ? "keep_all" : "rolling"},
        {"max_duration_ms", config.retention_policy.max_duration
                                ? Json(config.retention_policy.max_duration->count())
                                : Json(nullptr)},
        {"max_physical_bytes", config.retention_policy.max_physical_bytes
                                   ? Json(*config.retention_policy.max_physical_bytes)
                                   : Json(nullptr)}};
    Json pipelines = Json::array();
    for (const auto& pipeline : config.pipelines) {
        Json streams = Json::array();
        for (const auto& stream : pipeline.streams) {
            Json row{
                {"stream_key", stream.stream_key},
                {"readiness_policy", stream.readiness_policy == ReadinessPolicy::EntryReady
                                         ? "entry_ready"
                                         : "source_ready"}};
            if (stream.stream_id != 0) {
                row["stream_id"] = std::to_string(stream.stream_id);
            }
            if (!stream.name.empty()) {
                row["name"] = stream.name;
            }
            streams.push_back(std::move(row));
        }
        auto acq_cfg = take(persistence::parse_json(
            pipeline.acquisition.json, {limits.max_plugin_config_bytes, limits.max_json_depth}));
        auto stg_cfg = take(persistence::parse_json(
            pipeline.storage.json, {limits.max_plugin_config_bytes, limits.max_json_depth}));
        Json row{
            {"required", pipeline.required},
            {"resource_profile", pipeline.resource_profile},
            {"output_root", pipeline.output_root},
            {"acquisition",
             {{"plugin_id", pipeline.acquisition.plugin_id},
              {"plugin_version", pipeline.acquisition.plugin_version},
              {"config", std::move(acq_cfg)}}},
            {"storage",
             {{"plugin_id", pipeline.storage.plugin_id},
              {"plugin_version", pipeline.storage.plugin_version},
              {"config", std::move(stg_cfg)}}},
            {"streams", std::move(streams)}};
        if (pipeline.pipeline_id != 0) {
            row["pipeline_id"] = std::to_string(pipeline.pipeline_id);
        }
        pipelines.push_back(std::move(row));
    }
    Json root{
        {"format_version", "1.0"},
        {"segment_policy", std::move(segment_policy)},
        {"retention_policy", std::move(retention_policy)},
        {"pipelines", std::move(pipelines)}};
    if (!config.name.empty()) {
        root["name"] = config.name;
    }
    return root;
}

PluginConfig parse_plugin_obj(const Json& obj, const TaskConfigLimits& limits) {
    fields(obj, {"plugin_id", "plugin_version", "config"});
    require(obj.at("config").is_object(), "Plugin config must be object");
    PluginConfig out;
    out.plugin_id = req_string(obj, "plugin_id");
    out.plugin_version = req_string(obj, "plugin_version");
    out.json = obj.at("config").dump();
    require(out.json.size() <= limits.max_plugin_config_bytes, "Plugin config exceeds limit");
    return out;
}

}  // namespace

Result<TaskConfig> parse_task_config(
    std::string_view portable_json, const TaskConfigLimits& limits) {
    try {
        validate_limits(limits);
        const auto root = take(persistence::parse_json(
            portable_json, {limits.max_config_bytes, limits.max_json_depth}));
        fields(
            root, {"format_version", "segment_policy", "retention_policy", "pipelines"}, {"name"});
        if (req_string(root, "format_version") != "1.0") {
            return Status(StatusCode::InvalidArgument, "Unsupported TaskConfig format_version");
        }
        TaskConfig config;
        if (root.contains("name") && !root.at("name").is_null()) {
            config.name = req_string(root, "name");
            require(!config.name.empty(), "Task name must not be empty");
        }
        const auto& seg = root.at("segment_policy");
        fields(seg, {}, {"max_duration_ms", "max_physical_bytes"});
        if (const auto ms = nullable_safe_uint(seg, "max_duration_ms")) {
            config.segment_policy.max_duration =
                std::chrono::milliseconds{static_cast<std::int64_t>(*ms)};
        }
        config.segment_policy.max_physical_bytes = nullable_safe_uint(seg, "max_physical_bytes");

        const auto& ret = root.at("retention_policy");
        fields(ret, {"mode"}, {"max_duration_ms", "max_physical_bytes"});
        const auto& mode_str = req_string(ret, "mode");
        if (mode_str == "keep_all") {
            config.retention_policy.mode = RetentionMode::KeepAll;
        } else if (mode_str == "rolling") {
            config.retention_policy.mode = RetentionMode::Rolling;
        } else {
            return Status(StatusCode::InvalidArgument, "Unknown retention_policy.mode");
        }
        if (const auto ms = nullable_safe_uint(ret, "max_duration_ms")) {
            config.retention_policy.max_duration =
                std::chrono::milliseconds{static_cast<std::int64_t>(*ms)};
        }
        config.retention_policy.max_physical_bytes = nullable_safe_uint(ret, "max_physical_bytes");

        const auto& pipes = root.at("pipelines");
        require(pipes.is_array() && !pipes.empty() && pipes.size() <= limits.max_pipelines);
        config.pipelines.reserve(pipes.size());
        for (const auto& p : pipes) {
            fields(
                p,
                {"required", "resource_profile", "output_root", "acquisition", "storage",
                 "streams"},
                {"pipeline_id"});
            require(p.at("required").is_boolean(), "pipeline.required must be boolean");
            PipelineConfig pipe;
            pipe.pipeline_id = optional_id_field(p, "pipeline_id");
            pipe.required = p.at("required").get<bool>();
            pipe.resource_profile = req_string(p, "resource_profile");
            pipe.output_root = req_string(p, "output_root");
            pipe.acquisition = parse_plugin_obj(p.at("acquisition"), limits);
            pipe.storage = parse_plugin_obj(p.at("storage"), limits);

            const auto& streams = p.at("streams");
            require(
                streams.is_array() && !streams.empty() &&
                streams.size() <= limits.max_streams_per_pipeline);
            pipe.streams.reserve(streams.size());
            for (const auto& s : streams) {
                fields(s, {"stream_key", "readiness_policy"}, {"stream_id", "name"});
                StreamConfig stream;
                stream.stream_id = optional_id_field(s, "stream_id");
                stream.stream_key = req_string(s, "stream_key");
                if (s.contains("name") && !s.at("name").is_null()) {
                    stream.name = req_string(s, "name");
                    require(!stream.name.empty(), "Stream name must not be empty");
                }
                const auto& rp = req_string(s, "readiness_policy");
                if (rp == "entry_ready") {
                    stream.readiness_policy = ReadinessPolicy::EntryReady;
                } else if (rp == "source_ready") {
                    stream.readiness_policy = ReadinessPolicy::SourceReady;
                } else {
                    return Status(StatusCode::InvalidArgument, "Unknown readiness_policy");
                }
                pipe.streams.push_back(std::move(stream));
            }
            config.pipelines.push_back(std::move(pipe));
        }
        validate_config_impl(config, limits, false);
        return config;
    } catch (InvalidConfig& failure) {
        return std::move(failure.status);
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, "Invalid TaskConfig JSON");
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> export_task_config(const TaskConfig& config, const TaskConfigLimits& limits) {
    try {
        validate_config_impl(config, limits, false);
        auto dumped = to_json(config, limits).dump();
        if (dumped.size() > limits.max_config_bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        return dumped;
    } catch (InvalidConfig& failure) {
        return std::move(failure.status);
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

namespace task {

Result<void> validate_task_config(
    const TaskConfig& config, const TaskConfigLimits& limits, bool require_ids) {
    try {
        validate_config_impl(config, limits, require_ids);
        return {};
    } catch (InvalidConfig& failure) {
        return std::move(failure.status);
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<persistence::Json> config_document(
    const TaskConfig& config, const TaskConfigLimits& limits) {
    try {
        validate_config_impl(config, limits, false);
        return to_json(config, limits);
    } catch (InvalidConfig& failure) {
        return std::move(failure.status);
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

bool equal_task_config(const TaskConfig& left, const TaskConfig& right) noexcept {
    if (left.name != right.name ||
        left.segment_policy.max_duration != right.segment_policy.max_duration ||
        left.segment_policy.max_physical_bytes != right.segment_policy.max_physical_bytes ||
        left.retention_policy.mode != right.retention_policy.mode ||
        left.retention_policy.max_duration != right.retention_policy.max_duration ||
        left.retention_policy.max_physical_bytes != right.retention_policy.max_physical_bytes ||
        left.pipelines.size() != right.pipelines.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.pipelines.size(); ++i) {
        const auto& a = left.pipelines[i];
        const auto& b = right.pipelines[i];
        if (a.pipeline_id != b.pipeline_id || a.required != b.required ||
            a.resource_profile != b.resource_profile || a.output_root != b.output_root ||
            a.acquisition.plugin_id != b.acquisition.plugin_id ||
            a.acquisition.plugin_version != b.acquisition.plugin_version ||
            a.acquisition.json != b.acquisition.json ||
            a.storage.plugin_id != b.storage.plugin_id ||
            a.storage.plugin_version != b.storage.plugin_version ||
            a.storage.json != b.storage.json || a.streams.size() != b.streams.size()) {
            return false;
        }
        for (std::size_t j = 0; j < a.streams.size(); ++j) {
            const auto& sa = a.streams[j];
            const auto& sb = b.streams[j];
            if (sa.stream_id != sb.stream_id || sa.stream_key != sb.stream_key ||
                sa.name != sb.name || sa.readiness_policy != sb.readiness_policy) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace task
}  // namespace rodaq
