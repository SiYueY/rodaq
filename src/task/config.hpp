#pragma once

#include <string>

#include <rodaq/task/config.hpp>

#include "../persistence/json.hpp"

namespace rodaq::task {

persistence::JsonLimits config_json_limits(const TaskConfigLimits& limits);

Result<TaskConfig> normalize_config(const TaskConfig& config, const TaskConfigLimits& limits);
Result<persistence::Json> config_document(const TaskConfig& config, const TaskConfigLimits& limits);
Result<std::string> config_version(
    const TaskConfig& id_complete_config, const TaskConfigLimits& limits);

}  // namespace rodaq::task
