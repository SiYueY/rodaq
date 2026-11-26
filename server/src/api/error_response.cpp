#include "error_response.hpp"

#include <algorithm>

namespace rodaq::server {

std::string_view to_wire_string(Retryability value) noexcept {
    switch (value) {
        case Retryability::Never:
            return "never";
        case Retryability::AfterConditionChange:
            return "after_condition_change";
        case Retryability::AfterBackoff:
            return "after_backoff";
        case Retryability::AfterProcessRestart:
            return "after_process_restart";
        case Retryability::ManualRecoveryRequired:
            return "manual_recovery_required";
    }
    return "never";
}

std::string_view to_wire_string(FailureDomain value) noexcept {
    switch (value) {
        case FailureDomain::Request:
            return "request";
        case FailureDomain::TaskControl:
            return "task_control";
        case FailureDomain::Pipeline:
            return "pipeline";
        case FailureDomain::Collector:
            return "collector";
        case FailureDomain::Storage:
            return "storage";
        case FailureDomain::Volume:
            return "volume";
        case FailureDomain::PersistenceState:
            return "persistence_state";
        case FailureDomain::Process:
            return "process";
    }
    return "process";
}

std::string_view to_wire_string(DataImpact value) noexcept {
    switch (value) {
        case DataImpact::None:
            return "none";
        case DataImpact::PreAdmissionOnly:
            return "pre_admission_only";
        case DataImpact::AcceptedKnownFailed:
            return "accepted_known_failed";
        case DataImpact::PersistenceUnknown:
            return "persistence_unknown";
        case DataImpact::ArtifactQuarantined:
            return "artifact_quarantined";
        case DataImpact::HistoricalArtifactLostOrCorrupt:
            return "historical_artifact_lost_or_corrupt";
    }
    return "none";
}

std::string_view to_wire_string(DesiredState value) noexcept {
    switch (value) {
        case DesiredState::Stopped:
            return "stopped";
        case DesiredState::Running:
            return "running";
        case DesiredState::Paused:
            return "paused";
    }
    return "stopped";
}

std::string_view to_wire_string(ExecutionCondition value) noexcept {
    switch (value) {
        case ExecutionCondition::Idle:
            return "idle";
        case ExecutionCondition::Preparing:
            return "preparing";
        case ExecutionCondition::Active:
            return "active";
        case ExecutionCondition::Degraded:
            return "degraded";
        case ExecutionCondition::Draining:
            return "draining";
        case ExecutionCondition::Protected:
            return "protected";
        case ExecutionCondition::PendingCleanup:
            return "pending_cleanup";
        case ExecutionCondition::Failed:
            return "failed";
    }
    return "protected";
}

std::string_view to_wire_string(OperationKind value) noexcept {
    switch (value) {
        case OperationKind::Configure:
            return "configure";
        case OperationKind::Start:
            return "start";
        case OperationKind::Pause:
            return "pause";
        case OperationKind::Resume:
            return "resume";
        case OperationKind::Stop:
            return "stop";
        case OperationKind::Reset:
            return "reset";
        case OperationKind::Retire:
            return "retire";
    }
    return "stop";
}

std::string_view to_wire_string(OperationState value) noexcept {
    switch (value) {
        case OperationState::Accepted:
            return "accepted";
        case OperationState::InProgress:
            return "in_progress";
        case OperationState::Succeeded:
            return "succeeded";
        case OperationState::PartiallySucceeded:
            return "partially_succeeded";
        case OperationState::Failed:
            return "failed";
        case OperationState::Superseded:
            return "superseded";
    }
    return "failed";
}

std::string sanitize_bounded_message(std::string_view raw) {
    std::string out;
    out.reserve(std::min(raw.size(), kMaxDiagnosticBytes));
    for (const char ch : raw) {
        if (out.size() >= kMaxDiagnosticBytes) {
            break;
        }
        const auto byte = static_cast<unsigned char>(ch);
        if (byte >= 0x20 && byte != 0x7F) {
            out.push_back(ch);
        } else {
            out.push_back(' ');
        }
    }
    return out;
}

nlohmann::json make_api_version_json(ApiVersion version) {
    return nlohmann::json{{"major", version.major}, {"minor", version.minor}};
}

nlohmann::json make_success_envelope(
    const std::string& request_id, nlohmann::json data, ApiVersion version) {
    return nlohmann::json{
        {"api_version", make_api_version_json(version)},
        {"request_id", request_id},
        {"data", std::move(data)}};
}

nlohmann::json make_error_envelope(
    const std::string& request_id, ApiError error, ApiVersion version) {
    std::sort(
        error.details.begin(), error.details.end(), [](const ErrorDetail& a, const ErrorDetail& b) {
            if (a.path != b.path) {
                return a.path < b.path;
            }
            return a.code < b.code;
        });
    if (error.details.size() > kMaxErrorDetails) {
        error.details.resize(kMaxErrorDetails);
    }
    nlohmann::json details = nlohmann::json::array();
    for (const auto& d : error.details) {
        details.push_back(
            {{"code", d.code}, {"path", d.path}, {"message", sanitize_bounded_message(d.message)}});
    }
    return nlohmann::json{
        {"api_version", make_api_version_json(version)},
        {"request_id", request_id},
        {"error",
         {{"code", std::move(error.code)},
          {"message", sanitize_bounded_message(error.message)},
          {"retryability", to_wire_string(error.retryability)},
          {"failure_domain", to_wire_string(error.failure_domain)},
          {"data_impact", to_wire_string(error.data_impact)},
          {"details", std::move(details)}}}};
}

ApiError map_core_status(const Status& status, FailureDomain domain, bool is_operation_lookup) {
    ApiError err;
    err.failure_domain = domain;
    err.data_impact = DataImpact::None;
    switch (status.code()) {
        case StatusCode::InvalidArgument:
            err.http_status = 400;
            err.code = "invalid_argument";
            err.message = status.message().empty() ? "invalid request argument"
                                                   : std::string(status.message());
            err.retryability = Retryability::Never;
            err.failure_domain = FailureDomain::Request;
            break;
        case StatusCode::PermissionDenied:
            err.http_status = 403;
            err.code = "permission_denied";
            err.message = status.message().empty() ? "operation not permitted"
                                                   : std::string(status.message());
            err.retryability = Retryability::AfterConditionChange;
            break;
        case StatusCode::NotFound:
            err.http_status = 404;
            err.code = is_operation_lookup ? "operation_not_found_or_expired" : "not_found";
            err.message = is_operation_lookup ? "operation not found or expired from history"
                                              : "requested resource not found";
            err.retryability = Retryability::Never;
            break;
        case StatusCode::InvalidState:
            err.http_status = 409;
            err.code = "invalid_state";
            err.message = status.message().empty() ? "resource state does not allow operation"
                                                   : std::string(status.message());
            err.retryability = Retryability::AfterConditionChange;
            break;
        case StatusCode::Conflict:
            err.http_status = 409;
            err.code = "conflict";
            err.message = status.message().empty() ? "state or precondition conflict"
                                                   : std::string(status.message());
            err.retryability = Retryability::AfterConditionChange;
            break;
        case StatusCode::ResourceExhausted:
            err.http_status = 409;
            err.code = "resource_exhausted";
            err.message = status.message().empty() ? "business resource capacity exhausted"
                                                   : std::string(status.message());
            err.retryability = Retryability::AfterConditionChange;
            break;
        case StatusCode::DeadlineExceeded:
            err.http_status = 504;
            err.code = "observer_timeout";
            err.message = "observer deadline exceeded; retry with same Idempotency-Key";
            err.retryability = Retryability::AfterBackoff;
            break;
        case StatusCode::Unavailable:
            err.http_status = 503;
            err.code = "unavailable";
            err.message = status.message().empty() ? "service temporarily unavailable"
                                                   : std::string(status.message());
            err.retryability = Retryability::AfterBackoff;
            err.retry_after_seconds = 1;
            break;
        case StatusCode::DataLoss:
            err.http_status = 500;
            err.code = "data_loss";
            err.message = "durable state integrity violation detected";
            err.retryability = Retryability::ManualRecoveryRequired;
            err.failure_domain = FailureDomain::PersistenceState;
            err.data_impact = DataImpact::HistoricalArtifactLostOrCorrupt;
            break;
        case StatusCode::Internal:
        case StatusCode::Ok:
            err.http_status = 500;
            err.code = "internal_error";
            err.message = "internal server error";
            err.retryability = Retryability::AfterProcessRestart;
            err.failure_domain = FailureDomain::Process;
            break;
    }
    return err;
}

ApiError make_request_error(
    int http_status, std::string code, std::string message, std::vector<ErrorDetail> details) {
    ApiError err;
    err.http_status = http_status;
    err.code = std::move(code);
    err.message = std::move(message);
    err.failure_domain = FailureDomain::Request;
    err.retryability = (http_status == 429 || http_status == 503 || http_status == 504)
                           ? Retryability::AfterBackoff
                           : Retryability::Never;
    err.details = std::move(details);
    if (http_status == 429 || http_status == 503) {
        err.retry_after_seconds = 1;
    }
    return err;
}

}  // namespace rodaq::server
