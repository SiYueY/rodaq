#pragma once

#include "persistence/json.hpp"
#include "task/task_service.hpp"

#include <fstream>

namespace rodaq::test {

inline Result<TaskConfig> service_config() {
    std::ifstream input(RODAQ_TASK_CONFIG_FIXTURE);
    auto parsed = parse_task_config(persistence::Json::parse(input).dump());
    if (!parsed.ok()) {
        return parsed.status();
    }
    auto& pipeline = parsed.value().pipelines[0];
    pipeline.pipeline_id = 0;
    pipeline.streams[0].stream_id = 0;
    pipeline.acquisition = {"rodaq.fake.acquisition", "1.0.0", R"({"streams":["sample"]})"};
    pipeline.storage = {"rodaq.fake.storage", "1.0.0", "{}"};
    return parsed;
}

inline task::ServiceOptions service_options(
    std::filesystem::path root, std::shared_ptr<plugin::PluginManager> manager,
    std::shared_ptr<ExecutionLane> create_lane, std::shared_ptr<ExecutionLane> control_lane) {
    task::ServiceOptions options;
    options.root = std::move(root);
    options.plugin_manager = std::move(manager);
    options.create_lane = std::move(create_lane);
    options.control_lane = std::move(control_lane);
    options.resource_profiles = {{"basic", 100000}};
    options.output_roots = {"local"};
    options.allowed_plugins = {
        {"rodaq.fake.acquisition", "1.0.0", task::AdmissionRole::Acquisition},
        {"rodaq.fake.storage", "1.0.0", task::AdmissionRole::Storage}};
    return options;
}

inline Result<std::shared_ptr<plugin::PluginManager>> service_plugins(
    const std::filesystem::path& packages) {
    plugin::PluginManagerOptions options;
    options.search_paths = {packages};
    options.max_loaded_plugins = 2;
    options.max_instances = 4;
    options.max_config_size = 4096;
    options.shutdown_timeout = std::chrono::seconds{2};
    auto manager = plugin::PluginManager::create(std::move(options));
    if (!manager.ok()) {
        return manager.status();
    }
    return std::make_shared<plugin::PluginManager>(std::move(manager.value()));
}

}  // namespace rodaq::test
