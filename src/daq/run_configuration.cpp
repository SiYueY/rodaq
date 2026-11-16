#include "run_configuration.hpp"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

#include <rodaq/plugin/library.hpp>

#include "../pipeline/pipeline_controller.hpp"

namespace rodaq::daq {
namespace {

using persistence::Json;

Json binary(const plugin::PluginBinaryInfo& value) {
    return {
        {"plugin_id", value.plugin_id},
        {"plugin_version", value.plugin_version},
        {"sha256", value.sha256}};
}

Json limits(const ResourceProfile& profile) {
    const auto& i = profile.instance;
    const auto& s = profile.storage;
    const auto& m = profile.manifest;
    // Schema field names are internal identity inputs, not a second portable
    // Task/Deployment schema. No struct padding, ABI header or pointers enter it.
    return {
        {"instance",
         {{"records", std::to_string(i.records)},
          {"logical_bytes", std::to_string(i.logical_bytes)},
          {"queue_capacity", std::to_string(i.queue_capacity)},
          {"callback_concurrency", std::to_string(i.callback_concurrency)},
          {"sources_per_stream", std::to_string(i.sources_per_stream)},
          {"payloads",
           {{"physical_bytes", std::to_string(i.payloads.physical_bytes)},
            {"page_bytes", std::to_string(i.payloads.page_bytes)},
            {"max_payload_bytes", std::to_string(i.payloads.max_payload_bytes)},
            {"max_buffers", std::to_string(i.payloads.max_buffers)}}},
          {"descriptors",
           {{"max_descriptors_per_stream",
             std::to_string(i.descriptors.max_descriptors_per_stream)},
            {"max_schema_bytes", std::to_string(i.descriptors.max_schema_bytes)},
            {"max_offered_qos_profiles", std::to_string(i.descriptors.max_offered_qos_profiles)},
            {"max_owned_data_bytes", std::to_string(i.descriptors.max_owned_data_bytes)}}}}},
        {"storage",
         {{"max_record_bytes", std::to_string(s.max_record_bytes)},
          {"max_batch_bytes", std::to_string(s.max_batch_bytes)},
          {"max_batch_records", std::to_string(s.max_batch_records)},
          {"max_descriptors_per_segment", std::to_string(s.max_descriptors_per_segment)},
          {"max_segment_records", std::to_string(s.max_segment_records)},
          {"max_segment_input_bytes", std::to_string(s.max_segment_input_bytes)},
          {"max_segment_physical_bytes", std::to_string(s.max_segment_physical_bytes)},
          {"max_segment_file_count", std::to_string(s.max_segment_file_count)},
          {"max_relative_path_bytes", std::to_string(s.max_relative_path_bytes)}}},
        {"manifest",
         {{"max_bytes", std::to_string(m.max_bytes)},
          {"max_files", std::to_string(m.max_files)},
          {"max_descriptors", std::to_string(m.max_descriptors)},
          {"max_streams", std::to_string(m.max_streams)},
          {"max_epochs", std::to_string(m.max_epochs)},
          {"max_qos_profiles", std::to_string(m.max_qos_profiles)}}},
        {"max_prepared_descriptors", std::to_string(profile.max_prepared_descriptors)},
        {"sequence_reservation", std::to_string(profile.sequence_reservation)},
        {"descriptor_reservation", std::to_string(profile.descriptor_reservation)}};
}

}  // namespace

Result<Json> effective_configuration(
    const CompositionOptions& deployment, const task::TaskControlRecord& control,
    const PipelineConfig& definition, const ResourceProfile& profile,
    const plugin::PluginBinaryInfo& acquisition, const plugin::PluginBinaryInfo& storage) {
    try {
        auto source = persistence::parse_json(definition.acquisition.json);
        auto output = persistence::parse_json(definition.storage.json);
        if (!source.ok() || !output.ok()) {
            return !source.ok() ? source.status() : output.status();
        }
        const auto format = std::find_if(
            deployment.storage.begin(), deployment.storage.end(), [&](const auto& admitted) {
                return admitted.plugin_id == storage.plugin_id &&
                       admitted.plugin_version == storage.plugin_version;
            });
        if (format == deployment.storage.end()) {
            return Status(StatusCode::PermissionDenied, {});
        }
        return Json{
            {"config_version", control.active_config_version},
            {"pipeline_id", std::to_string(definition.pipeline_id)},
            {"deployment_generation", std::to_string(deployment.deployment_generation)},
            {"qualified_runtime_profile_id", deployment.qualified_runtime_profile_id},
            {"acquisition", binary(acquisition)},
            {"storage", binary(storage)},
            {"volume_id", artifact::to_string(deployment.volume->identity())},
            {"format", format->format},
            {"resolved_acquisition", std::move(source.value())},
            {"resolved_storage", std::move(output.value())},
            {"effective_pipeline_limits", limits(profile)},
            {"native_timeout_ns", std::to_string(deployment.native_timeout.count())},
            // V1 supplies no secret-handoff ABI. No credential references or
            // Deployment overrides are admitted by this S2 composition.
            {"credential_reference_versions", Json::object()}};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<task::RunInputs> resolve_run(
    const RunResources& resources, const task::TaskControlRecord& control, const TaskConfig& config,
    pipeline::StreamIdentityStore& identities) {
    try {
        const auto& options = resources.options;
        if (config.pipelines.size() > resources.writers.size()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        task::RunInputs result;
        result.boot = options.boot;
        result.sample = options.sample;
        result.pipelines.reserve(config.pipelines.size());
        for (const auto& definition : config.pipelines) {
            const auto profile = std::find_if(
                options.profiles.begin(), options.profiles.end(),
                [&](const auto& value) { return value.name == definition.resource_profile; });
            if (profile == options.profiles.end() ||
                definition.output_root != options.output_root) {
                return Status(StatusCode::PermissionDenied, {});
            }
            auto source = options.plugins->load(
                definition.acquisition.plugin_id, definition.acquisition.plugin_version);
            auto output = options.plugins->load(
                definition.storage.plugin_id, definition.storage.plugin_version);
            if (!source.ok() || !output.ok()) {
                return !source.ok() ? source.status() : output.status();
            }
            auto frozen = effective_configuration(
                options, control, definition, *profile, source.value().binary(),
                output.value().binary());
            if (!frozen.ok()) {
                return frozen.status();
            }
            auto effective =
                persistence::semantic_digest(frozen.value(), "rodaq.effective-config.v1");
            if (!effective.ok()) {
                return effective.status();
            }
            auto instance = pipeline::new_instance_id();
            if (!instance.ok()) {
                return instance.status();
            }
            pipeline::PipelineOptions selected;
            selected.identity = {
                control.task_id, definition.pipeline_id, instance.value(),
                control.control_generation};
            selected.definition = definition;
            selected.limits = profile->instance;
            selected.envelope = profile->storage;
            selected.manifest_limits = profile->manifest;
            selected.max_prepared_descriptors = profile->max_prepared_descriptors;
            for (const auto& stream : definition.streams) {
                auto records = identities.reserve(stream.stream_id, profile->sequence_reservation);
                if (!records.ok()) {
                    return records.status();
                }
                auto descriptors = identities.reserve_descriptors(
                    stream.stream_id, profile->descriptor_reservation);
                if (!descriptors.ok()) {
                    return descriptors.status();
                }
                selected.streams.push_back(
                    {stream.stream_id, stream.stream_key, profile->storage.max_record_bytes,
                     records.value(), descriptors.value()});
            }
            auto segment = options.segment_ids->reserve(1);
            if (!segment.ok()) {
                return segment.status();
            }
            const auto format = std::find_if(
                options.storage.begin(), options.storage.end(), [&](const auto& value) {
                    return value.plugin_id == definition.storage.plugin_id &&
                           value.plugin_version == definition.storage.plugin_version;
                });
            selected.manifest = {
                segment.value().begin,
                control.task_id,
                definition.pipeline_id,
                pipeline::to_string(instance.value()),
                1,
                control.active_config_version,
                effective.value(),
                control.control_generation,
                control.reset_generation,
                options.volume->identity(),
                source.value().binary(),
                output.value().binary(),
                options.qualified_runtime_profile_id,
                format->format,
                options.clock->anchor()};
            selected.plugins = options.plugins;
            selected.writer_lane = resources.writers[result.pipelines.size()];
            selected.clock = options.clock;
            selected.volume = options.volume;
            selected.capacity = options.capacity;
            selected.ledger = options.ledger;
            selected.receive_clock = options.sample;
            selected.native_timeout = options.native_timeout;
            auto controller = pipeline::PipelineController::create(std::move(selected));
            if (!controller.ok()) {
                return controller.status();
            }
            result.pipelines.push_back(
                {std::move(controller.value()), std::move(effective.value()), definition.required});
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::daq
