#include "config_identity.hpp"

#include "config.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string_view>
#include <utility>

namespace rodaq::task {
namespace {

struct IdExhausted {};

std::uint64_t next_id(std::uint64_t& high_water) {
    if (high_water == std::numeric_limits<std::uint64_t>::max()) {
        throw IdExhausted{};
    }
    return ++high_water;
}

// With bounded input sets this skips at most input cardinality entries, even
// when an explicit imported identity is UINT64_MAX.
std::uint64_t initial_id(std::uint64_t& next, const std::set<std::uint64_t>& explicit_ids) {
    while (explicit_ids.count(next) != 0) {
        next_id(next);
    }
    const auto assigned = next;
    next_id(next);
    return assigned;
}

}  // namespace

Result<AssignedConfig> assign_initial_config(
    const TaskConfig& request, const TaskConfigLimits& limits) {
    auto normalized = normalize_config(request, limits);
    if (!normalized.ok()) {
        return std::move(normalized).status();
    }
    try {
        AssignedConfig result{std::move(normalized).value(), 0, 0};
        std::set<PipelineId> explicit_pipelines;
        std::set<StreamId> explicit_streams;
        for (const auto& pipeline : result.config.pipelines) {
            if (pipeline.pipeline_id != 0) {
                explicit_pipelines.insert(pipeline.pipeline_id);
            }
            for (const auto& stream : pipeline.streams) {
                if (stream.stream_id != 0) {
                    explicit_streams.insert(stream.stream_id);
                }
            }
        }
        PipelineId next_pipeline = 1;
        StreamId next_stream = 1;
        for (auto& pipeline : result.config.pipelines) {
            if (pipeline.pipeline_id == 0) {
                pipeline.pipeline_id = initial_id(next_pipeline, explicit_pipelines);
            }
            result.pipeline_id_high_water =
                std::max(result.pipeline_id_high_water, pipeline.pipeline_id);
            for (auto& stream : pipeline.streams) {
                if (stream.stream_id == 0) {
                    stream.stream_id = initial_id(next_stream, explicit_streams);
                }
                result.stream_id_high_water =
                    std::max(result.stream_id_high_water, stream.stream_id);
            }
        }
        return result;
    } catch (const IdExhausted&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<AssignedConfig> assign_config_change(
    const TaskConfig& current, const TaskConfig& request, PipelineId pipeline_id_high_water,
    StreamId stream_id_high_water, const TaskConfigLimits& limits) {
    auto old_config = normalize_config(current, limits);
    auto normalized = normalize_config(request, limits);
    if (!old_config.ok()) {
        return std::move(old_config).status();
    }
    if (!normalized.ok()) {
        return std::move(normalized).status();
    }
    try {
        std::set<PipelineId> pipelines;
        std::map<StreamId, std::string_view> streams;
        for (const auto& pipeline : old_config.value().pipelines) {
            if (pipeline.pipeline_id == 0 || pipeline.pipeline_id > pipeline_id_high_water) {
                return Status(
                    StatusCode::DataLoss, "Pipeline identity authority is behind active Config");
            }
            pipelines.insert(pipeline.pipeline_id);
            for (const auto& stream : pipeline.streams) {
                if (stream.stream_id == 0 || stream.stream_id > stream_id_high_water) {
                    return Status(
                        StatusCode::DataLoss, "Stream identity authority is behind active Config");
                }
                streams.emplace(stream.stream_id, stream.stream_key);
            }
        }
        AssignedConfig result{
            std::move(normalized).value(), pipeline_id_high_water, stream_id_high_water};
        // Validate every retained identity before allocating any new identity.
        for (const auto& pipeline : result.config.pipelines) {
            if (pipeline.pipeline_id != 0 && pipelines.count(pipeline.pipeline_id) == 0) {
                return Status(StatusCode::InvalidArgument, "Unknown existing Pipeline ID");
            }
            for (const auto& stream : pipeline.streams) {
                if (stream.stream_id != 0) {
                    const auto found = streams.find(stream.stream_id);
                    if (found == streams.end() || found->second != stream.stream_key) {
                        return Status(StatusCode::InvalidArgument, "Unknown or rebound Stream ID");
                    }
                }
            }
        }
        for (auto& pipeline : result.config.pipelines) {
            if (pipeline.pipeline_id == 0) {
                pipeline.pipeline_id = next_id(result.pipeline_id_high_water);
            }
            for (auto& stream : pipeline.streams) {
                if (stream.stream_id == 0) {
                    stream.stream_id = next_id(result.stream_id_high_water);
                }
            }
        }
        return result;
    } catch (const IdExhausted&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<pipeline::StreamIdentityProjection> identity_projection(
    const AssignedConfig& assigned, const TaskConfigLimits& limits) {
    auto version = config_version(assigned.config, limits);
    if (!version.ok()) {
        return std::move(version).status();
    }
    try {
        pipeline::StreamIdentityProjection projection;
        projection.config_version = std::move(version).value();
        projection.pipeline_id_high_water = assigned.pipeline_id_high_water;
        projection.stream_id_high_water = assigned.stream_id_high_water;
        for (const auto& pipeline_config : assigned.config.pipelines) {
            if (pipeline_config.pipeline_id > assigned.pipeline_id_high_water) {
                return Status(StatusCode::InvalidArgument, {});
            }
            for (const auto& stream : pipeline_config.streams) {
                projection.streams.push_back({stream.stream_id, stream.stream_key});
            }
        }
        std::sort(
            projection.streams.begin(), projection.streams.end(),
            [](const auto& left, const auto& right) { return left.stream_id < right.stream_id; });
        auto valid = pipeline::validate_identity_projection(projection, projection.streams.size());
        if (!valid.ok()) {
            return std::move(valid).status();
        }
        return projection;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::task
