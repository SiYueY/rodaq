#pragma once

#include <rodaq/task/config.hpp>

#include "../persistence/json.hpp"

namespace rodaq::task {

Result<void> validate_task_config(
    const TaskConfig& config, const TaskConfigLimits& limits = {}, bool require_ids = false);
Result<persistence::Json> config_document(
    const TaskConfig& config, const TaskConfigLimits& limits = {});
bool equal_task_config(const TaskConfig& left, const TaskConfig& right) noexcept;

}  // namespace rodaq::task
