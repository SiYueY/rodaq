#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <rodaq/task/types.hpp>

#include "operation_history.hpp"

namespace rodaq::task {

struct ResetIntent {
    OperationId operation_id;
    std::uint64_t reset_generation{0};
    ControlGeneration target_control_generation{0};
    // Scope is all Task-owned recording artifacts through the fixed control
    // boundary, across managed volumes; Recovery owns only execution progress.
};

struct TaskControlRecord {
    TaskId task_id;
    std::string active_config_version;
    DesiredState desired_state{DesiredState::Stopped};
    ControlGeneration control_generation{1};
    std::uint64_t reset_generation{0};
    // Status/terminal publications also advance this revision. They never
    // manufacture a new user intent or change its control generation.
    std::uint64_t record_revision{1};
    std::optional<OperationId> current_operation_id;
    OperationHistory operation_history;
    std::optional<ResetIntent> reset_intent;
};

std::size_t control_record_max_bytes(const HistoryPolicy& policy) noexcept;

Result<void> validate_control_record(const TaskControlRecord& record, const HistoryPolicy& policy);
Result<std::string> encode_control_record(
    const TaskControlRecord& record, const HistoryPolicy& policy);
Result<TaskControlRecord> decode_control_record(
    std::string_view bytes, const HistoryPolicy& policy);

}  // namespace rodaq::task
