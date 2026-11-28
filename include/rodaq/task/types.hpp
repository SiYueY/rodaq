#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/result.hpp>
#include <rodaq/task/config.hpp>

namespace rodaq {

struct TaskId {
    std::array<std::uint8_t, 16> value{};

    friend bool operator==(const TaskId& left, const TaskId& right) noexcept {
        return left.value == right.value;
    }

    friend bool operator!=(const TaskId& left, const TaskId& right) noexcept {
        return left.value != right.value;
    }

    friend bool operator<(const TaskId& left, const TaskId& right) noexcept {
        return left.value < right.value;
    }
};

struct OperationId {
    std::array<std::uint8_t, 16> value{};

    friend bool operator==(const OperationId& left, const OperationId& right) noexcept {
        return left.value == right.value;
    }

    friend bool operator!=(const OperationId& left, const OperationId& right) noexcept {
        return left.value != right.value;
    }

    friend bool operator<(const OperationId& left, const OperationId& right) noexcept {
        return left.value < right.value;
    }
};

Result<TaskId> parse_task_id(std::string_view text);
Result<OperationId> parse_operation_id(std::string_view text);
std::string to_string(TaskId task_id);
std::string to_string(OperationId operation_id);

enum class DesiredState { Stopped, Running, Paused };
enum class ExecutionCondition { Idle, Preparing, Active, Degraded, Draining, Protected, Failed };
enum class OperationKind { Configure, Start, Pause, Resume, Stop, Reset, Retire };
enum class OperationState {
    Accepted,
    InProgress,
    Succeeded,
    PartiallySucceeded,
    Failed,
    Superseded
};
enum class Retryability {
    Never,
    AfterConditionChange,
    AfterBackoff,
    AfterProcessRestart,
    ManualRecoveryRequired
};
enum class FailureDomain {
    Request,
    TaskControl,
    Pipeline,
    Collector,
    Storage,
    Volume,
    PersistenceState,
    Process
};
enum class DataImpact {
    None,
    PreAdmissionOnly,
    AcceptedKnownFailed,
    PersistenceUnknown,
    ArtifactUnavailableOrCorrupt
};

using WallTime = std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

struct RequestIdentity {
    std::string request_key;
};

struct ControlPrecondition {
    bool has_expected_control_generation{false};
    ControlGeneration expected_control_generation{0};
};

struct OperationSnapshot {
    OperationId operation_id;
    OperationKind kind{OperationKind::Stop};
    OperationState state{OperationState::Accepted};
    ControlGeneration control_generation{0};
    WallTime accepted_at;
    std::optional<WallTime> terminal_at;
    Retryability retryability{Retryability::Never};
    FailureDomain failure_domain{FailureDomain::TaskControl};
    DataImpact data_impact{DataImpact::None};
    StatusCode status_code{StatusCode::Ok};
    std::string diagnostic;
};

struct PageRequest {
    std::string cursor;
    std::uint32_t limit{50};
};

struct OperationPage {
    std::vector<OperationSnapshot> items;
    bool has_more{false};
    std::string next_cursor;
    bool history_truncated{false};
};

}  // namespace rodaq
