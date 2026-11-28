#include "run_configuration.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

#include <rodaq/plugin/library.hpp>

#include "../pipeline/pipeline_controller.hpp"

namespace rodaq::daq {

artifact::ManifestLimits catalog_manifest_limits(const DeploymentSettings& settings) {
    if (settings.profiles.empty()) {
        return {};
    }
    auto limits = settings.profiles.front().manifest;
    for (const auto& profile : settings.profiles) {
        const auto& selected = profile.manifest;
        limits.max_bytes = std::max(limits.max_bytes, selected.max_bytes);
        limits.max_files = std::max(limits.max_files, selected.max_files);
        limits.max_descriptors = std::max(limits.max_descriptors, selected.max_descriptors);
        limits.max_streams = std::max(limits.max_streams, selected.max_streams);
        limits.max_descriptor_metadata_bytes =
            std::max(limits.max_descriptor_metadata_bytes, selected.max_descriptor_metadata_bytes);
    }
    return limits;
}

void release_writer_slot(const RunResources& resources, TaskId task_id) noexcept {
    const auto width = resources.options.settings.tasks.config.max_pipelines;
    std::lock_guard<std::mutex> lock(resources.writer_ownership);
    for (std::size_t slot = 0; slot < resources.writer_tasks.size(); ++slot) {
        if (!resources.writer_tasks[slot].has_value() ||
            !(*resources.writer_tasks[slot] == task_id)) {
            continue;
        }
        const std::size_t base = slot * width;
        bool idle = true;
        for (std::size_t i = 0; i < width && base + i < resources.writers.size(); ++i) {
            const auto snap = resources.writers[base + i]->snapshot();
            if (snap.running != 0 || snap.queued != 0) {
                idle = false;
                break;
            }
        }
        if (idle) {
            resources.writer_tasks[slot].reset();
        }
        return;
    }
}

Result<task::RunInputs> resolve_run(
    const RunResources& resources, TaskId task_id, std::string_view config_version,
    ControlGeneration control_generation, const TaskConfig& config) {
    try {
        const auto& options = resources.options;
        const auto width = options.settings.tasks.config.max_pipelines;
        if (width == 0 || options.settings.max_tasks == 0 || config.pipelines.size() > width ||
            resources.writers.size() / width < options.settings.max_tasks) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        std::size_t writer_offset = 0;
        {
            std::lock_guard<std::mutex> lock(resources.writer_ownership);
            if (resources.writer_tasks.size() < options.settings.max_tasks) {
                resources.writer_tasks.resize(options.settings.max_tasks);
            }
            std::optional<std::size_t> selected_slot;
            std::optional<std::size_t> free_slot;
            for (std::size_t i = 0; i < options.settings.max_tasks; ++i) {
                if (resources.writer_tasks[i].has_value() &&
                    *resources.writer_tasks[i] == task_id) {
                    selected_slot = i;
                    break;
                }
                if (!free_slot && !resources.writer_tasks[i].has_value()) {
                    free_slot = i;
                }
            }
            if (!selected_slot) {
                if (!free_slot) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                resources.writer_tasks[*free_slot] = task_id;
                selected_slot = *free_slot;
            }
            writer_offset = *selected_slot * width;
        }
        task::RunInputs result;
        result.pipelines.reserve(config.pipelines.size());
        for (const auto& definition : config.pipelines) {
            const auto profile = std::find_if(
                options.settings.profiles.begin(), options.settings.profiles.end(),
                [&](const auto& value) { return value.name == definition.resource_profile; });
            if (profile == options.settings.profiles.end() ||
                definition.output_root != options.settings.output_root) {
                return Status(StatusCode::PermissionDenied, {});
            }
            const auto format = std::find_if(
                options.settings.storage.begin(), options.settings.storage.end(),
                [&](const auto& value) {
                    return value.plugin_id == definition.storage.plugin_id &&
                           value.plugin_version == definition.storage.plugin_version;
                });
            if (format == options.settings.storage.end()) {
                return Status(StatusCode::PermissionDenied, {});
            }
            auto source = options.plugins->load(
                definition.acquisition.plugin_id, definition.acquisition.plugin_version);
            auto output = options.plugins->load(
                definition.storage.plugin_id, definition.storage.plugin_version);
            if (!source.ok() || !output.ok()) {
                return !source.ok() ? source.status() : output.status();
            }
            auto instance = pipeline::new_instance_id();
            if (!instance.ok()) {
                return instance.status();
            }
            auto segment_id = options.volume->allocate_segment_id();
            if (!segment_id.ok()) {
                return segment_id.status();
            }
            pipeline::PipelineOptions selected;
            selected.identity = {
                task_id, definition.pipeline_id, instance.value(), control_generation};
            selected.definition = definition;
            selected.limits = profile->instance;
            selected.envelope = profile->storage;
            selected.segment_reservation_bytes = profile->segment_reservation_bytes;
            selected.manifest_limits = profile->manifest;
            selected.max_prepared_descriptors = profile->max_prepared_descriptors;
            selected.streams.reserve(definition.streams.size());
            for (const auto& stream : definition.streams) {
                selected.streams.push_back(
                    {stream.stream_id, stream.stream_key, profile->storage.max_record_bytes});
            }
            selected.manifest.segment_id = segment_id.value();
            selected.manifest.task_id = task_id;
            selected.manifest.pipeline_id = definition.pipeline_id;
            selected.manifest.instance_id = pipeline::to_string(instance.value());
            selected.manifest.segment_sequence = 1;
            selected.manifest.config_version = std::string(config_version);
            selected.manifest.deployment_generation =
                std::to_string(options.settings.deployment_generation);
            selected.manifest.control_generation = control_generation;
            selected.manifest.volume_id = options.volume->identity();
            selected.manifest.acquisition = source.value().info();
            selected.manifest.storage = output.value().info();
            selected.manifest.qualified_runtime_profile_id =
                options.settings.qualified_runtime_profile_id;
            selected.manifest.format = format->format;
            selected.plugins = options.plugins;
            selected.writer_lane = resources.writers[writer_offset + result.pipelines.size()];
            selected.volume = options.volume;
            selected.capacity = options.capacity;
            selected.receive_clock = options.sample;
            selected.native_timeout = options.settings.native_timeout;
            auto controller = pipeline::PipelineController::create(std::move(selected));
            if (!controller.ok()) {
                return controller.status();
            }
            task::RunPipeline pipe;
            pipe.controller = std::move(controller.value());
            pipe.required = definition.required;
            result.pipelines.push_back(std::move(pipe));
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::daq
