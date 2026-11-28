#pragma once

#include "../config.hpp"
#include "error_response.hpp"
#include "json_codec.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/task_service.hpp>

namespace rodaq::server {

struct OperationRequestDto {
    OperationKind kind{OperationKind::Start};
};

std::string_view to_wire_string(VolumePressure pressure) noexcept;
std::string_view to_wire_string(BackendRole role) noexcept;
std::string_view to_wire_string(SupportStatus status) noexcept;
std::string_view to_wire_string(QualificationStatus status) noexcept;
std::string_view to_wire_string(RecordRejectionReason reason) noexcept;

nlohmann::json serialize_freshness(SnapshotFreshness freshness, std::chrono::nanoseconds age);

nlohmann::json serialize_system(
    ServerPhase phase, const SystemSnapshot& snapshot, std::uint64_t active_downloads,
    const std::optional<std::string>& deployment_error = std::nullopt);

nlohmann::json serialize_capabilities(
    const SystemCapabilities& caps, const SystemSnapshot& snapshot, const ServerConfig& config);

nlohmann::json serialize_task_data(const TaskSnapshot& snapshot);
nlohmann::json serialize_task_create(TaskId task_id, const std::string& config_version);
nlohmann::json serialize_task_page(const TaskPage& page);
nlohmann::json serialize_mutation_accepted(TaskId task_id, OperationId operation_id);
nlohmann::json serialize_operation(TaskId task_id, const OperationSnapshot& op);
nlohmann::json serialize_operation_page(TaskId task_id, const OperationPage& page);
nlohmann::json serialize_task_runtime(const TaskRuntimeSnapshot& runtime);
nlohmann::json serialize_artifact_summary(const ArtifactSummary& summary);
nlohmann::json serialize_artifact_detail(const ArtifactInfo& info);
nlohmann::json serialize_artifact_page(const ArtifactPage& page);

Result<OperationRequestDto> parse_operation_request(
    std::string_view body, const JsonParseLimits& limits, ApiError& error_out);

}  // namespace rodaq::server
