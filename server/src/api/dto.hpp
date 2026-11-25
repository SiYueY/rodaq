#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <rodaq/artifact/artifact_service.hpp>
#include <rodaq/status.hpp>
#include <rodaq/system/system_service.hpp>
#include <rodaq/task/config.hpp>
#include <rodaq/task/runtime.hpp>
#include <rodaq/task/task_service.hpp>
#include <rodaq/task/types.hpp>

#include "../security/auth.hpp"
#include "../server_config.hpp"

namespace rodaq::server {

enum class ServerPhase { Recovering, Ready, Draining, Failed };

struct ErrorDetail {
    std::string code;
    std::string path;
    std::string message;
};

struct MappedError {
    drogon::HttpStatusCode http_status{drogon::k500InternalServerError};
    std::string code{"internal"};
    std::string message;
    std::string retryability{"never"};
    std::string failure_domain{"request"};
    std::string data_impact{"none"};
    std::vector<ErrorDetail> details;
};

std::string to_string(ServerPhase phase);
std::string to_string(DesiredState state);
std::string to_string(ExecutionCondition condition);
std::string to_string(OperationKind kind);
std::string to_string(OperationState state);
std::string to_string(Retryability retry);
std::string to_string(FailureDomain domain);
std::string to_string(DataImpact impact);
std::string to_string(SnapshotFreshness freshness);
std::string to_string(VolumePressure pressure);
std::string to_string(RecordRejectionReason reason);

std::string format_rfc3339_utc(WallTime tp);
Result<WallTime> parse_rfc3339_utc(std::string_view text);
nlohmann::json duration_ns_json(std::uint64_t ns);
nlohmann::json freshness_json(SnapshotFreshness state, std::optional<std::chrono::nanoseconds> age);

Result<ApiVersion> parse_api_version_header(std::string_view header);
Result<std::uint64_t> parse_decimal_header_u64(std::string_view text);
bool valid_idempotency_key(std::string_view key) noexcept;

// Strict OperationRequest parser: {"kind": "start|pause|resume|stop|reset|retire", "parameters":
// {}}
Result<OperationKind> parse_lifecycle_operation_request(std::string_view body);

MappedError map_core_status(
    const Status& status, std::string_view default_domain = "task_control",
    bool operation_lookup = false);

nlohmann::json make_success_envelope(
    ApiVersion version, std::string_view request_id, nlohmann::json data);
nlohmann::json make_error_envelope(
    ApiVersion version, std::string_view request_id, const MappedError& err);

void apply_common_security_headers(
    const drogon::HttpResponsePtr& resp, ApiVersion version, std::string_view request_id,
    bool no_store = true);

drogon::HttpResponsePtr make_json_response(
    drogon::HttpStatusCode status, ApiVersion version, std::string_view request_id,
    const nlohmann::json& body, bool no_store = true);

drogon::HttpResponsePtr make_error_response(
    ApiVersion version, std::string_view request_id, const MappedError& err);

nlohmann::json to_session_dto(const SessionSnapshot& snap);
nlohmann::json to_session_dto(const AuthContext& auth);
nlohmann::json to_system_dto(
    ServerPhase phase, const SystemSnapshot& snap, std::size_t active_downloads,
    std::optional<std::string> web_error = std::nullopt);
nlohmann::json to_capabilities_dto(const SystemCapabilities& caps, const ServerConfig& config);
nlohmann::json to_task_dto(
    const TaskSnapshot& snap, const std::optional<std::string>& name,
    SnapshotFreshness freshness = SnapshotFreshness::Fresh);
nlohmann::json to_task_summary_dto(const TaskSummary& summary);
nlohmann::json to_task_page_dto(const TaskPage& page);
nlohmann::json to_operation_dto(TaskId task_id, const OperationSnapshot& snap);
nlohmann::json to_operation_page_dto(TaskId task_id, const OperationPage& page);
nlohmann::json to_task_runtime_dto(const TaskRuntimeSnapshot& snap);
nlohmann::json to_artifact_summary_dto(const ArtifactSummary& summary);
nlohmann::json to_artifact_page_dto(const ArtifactPage& page);
nlohmann::json to_artifact_detail_dto(const ArtifactInfo& info);

}  // namespace rodaq::server
