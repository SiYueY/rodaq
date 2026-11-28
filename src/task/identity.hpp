#pragma once

#include <string>

#include <rodaq/task/types.hpp>

namespace rodaq::task {

Result<TaskId> new_task_id();
Result<OperationId> new_operation_id();
Result<std::string> new_config_version();
Result<void> validate_request_identity(const RequestIdentity& request);

}  // namespace rodaq::task
