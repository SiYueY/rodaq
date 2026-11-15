#pragma once

#include <string>

#include <rodaq/task/types.hpp>

namespace rodaq::task {

Result<TaskId> new_task_id();
Result<OperationId> new_operation_id();
Result<void> validate_request_identity(const RequestIdentity& request);
Result<std::string> request_key_hash(const RequestIdentity& request);
Result<std::string> create_request_digest(const TaskConfig& config, const TaskConfigLimits& limits);
Result<std::string> configure_request_digest(
    TaskId task_id, const TaskConfig& config, const ControlPrecondition& precondition,
    const TaskConfigLimits& limits);
Result<std::string> lifecycle_request_digest(
    TaskId task_id, OperationKind kind, const ControlPrecondition& precondition);

}  // namespace rodaq::task
