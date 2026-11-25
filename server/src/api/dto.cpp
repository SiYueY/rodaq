#include "dto.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>
#include <string>

namespace rodaq::server {
namespace {

using Json = nlohmann::json;

double sanitize_rate(double val) noexcept {
    if (!std::isfinite(val) || val < 0.0) {
        return 0.0;
    }
    return val;
}

}  // namespace

std::string to_string(ServerPhase phase) {
    switch (phase) {
        case ServerPhase::Recovering:
            return "recovering";
        case ServerPhase::Ready:
            return "ready";
        case ServerPhase::Draining:
            return "draining";
        case ServerPhase::Failed:
            return "failed";
    }
    return "failed";
}

std::string to_string(DesiredState state) {
    switch (state) {
        case DesiredState::Stopped:
            return "stopped";
        case DesiredState::Running:
            return "running";
        case DesiredState::Paused:
            return "paused";
    }
    return "stopped";
}

std::string to_string(ExecutionCondition condition) {
    switch (condition) {
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

std::string to_string(OperationKind kind) {
    switch (kind) {
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

std::string to_string(OperationState state) {
    switch (state) {
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

std::string to_string(Retryability retry) {
    switch (retry) {
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

std::string to_string(FailureDomain domain) {
    switch (domain) {
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
    return "task_control";
}

std::string to_string(DataImpact impact) {
    switch (impact) {
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

std::string to_string(SnapshotFreshness freshness) {
    switch (freshness) {
        case SnapshotFreshness::Fresh:
            return "fresh";
        case SnapshotFreshness::Stale:
            return "stale";
        case SnapshotFreshness::NotObserved:
            return "unknown";
    }
    return "unknown";
}

std::string to_string(VolumePressure pressure) {
    switch (pressure) {
        case VolumePressure::Normal:
            return "normal";
        case VolumePressure::Low:
            return "low";
        case VolumePressure::Critical:
            return "critical";
        case VolumePressure::Protected:
            return "protected";
    }
    return "protected";
}

std::string to_string(RecordRejectionReason reason) {
    switch (reason) {
        case RecordRejectionReason::GateClosed:
            return "gate_closed";
        case RecordRejectionReason::ObsoleteInstance:
            return "obsolete_instance";
        case RecordRejectionReason::StaleDescriptor:
            return "stale_descriptor";
        case RecordRejectionReason::DescriptorNotReady:
            return "descriptor_not_ready";
        case RecordRejectionReason::InvalidRecord:
            return "invalid_record";
        case RecordRejectionReason::RecordTooLarge:
            return "record_too_large";
        case RecordRejectionReason::ResourceExhausted:
            return "resource_exhausted";
    }
    return "invalid_record";
}

std::string format_rfc3339_utc(WallTime tp) {
    const auto ns_total = tp.time_since_epoch().count();
    std::int64_t secs = ns_total / 1'000'000'000;
    std::int64_t rem_ns = ns_total % 1'000'000'000;
    if (rem_ns < 0) {
        --secs;
        rem_ns += 1'000'000'000;
    }
    const std::time_t tt = static_cast<std::time_t>(secs);
    std::tm utc{};
    gmtime_r(&tt, &utc);
    char buf[48]{};
    if (rem_ns == 0) {
        std::snprintf(
            buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", utc.tm_year + 1900, utc.tm_mon + 1,
            utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else {
        std::snprintf(
            buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ", utc.tm_year + 1900,
            utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
            static_cast<long long>(rem_ns));
    }
    return std::string(buf);
}

Result<WallTime> parse_rfc3339_utc(std::string_view text) {
    if (text.size() < 20 || text.back() != 'Z') {
        return Status(StatusCode::InvalidArgument, "Invalid RFC3339 UTC timestamp");
    }
    std::tm tm{};
    const auto parse_int = [&](std::size_t pos, std::size_t len, int& out) -> bool {
        if (pos + len > text.size()) {
            return false;
        }
        const auto [ptr, ec] = std::from_chars(text.data() + pos, text.data() + pos + len, out);
        return ec == std::errc{} && ptr == text.data() + pos + len;
    };
    int year = 0;
    int mon = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        !parse_int(0, 4, year) || !parse_int(5, 2, mon) || !parse_int(8, 2, day) ||
        !parse_int(11, 2, hour) || !parse_int(14, 2, min) || !parse_int(17, 2, sec)) {
        return Status(StatusCode::InvalidArgument, "Malformed RFC3339 UTC timestamp");
    }
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || min < 0 ||
        min > 59 || sec < 0 || sec > 59) {
        return Status(StatusCode::InvalidArgument, "RFC3339 component out of range");
    }
    std::int64_t frac_ns = 0;
    if (text.size() > 20) {
        if (text[19] != '.') {
            return Status(StatusCode::InvalidArgument, "Invalid fractional seconds");
        }
        const auto frac = text.substr(20, text.size() - 21);
        if (frac.empty() || frac.size() > 9) {
            return Status(StatusCode::InvalidArgument, "Fractional seconds length invalid");
        }
        std::int64_t mult = 100'000'000;
        for (char c : frac) {
            if (c < '0' || c > '9') {
                return Status(StatusCode::InvalidArgument, "Non-digit fractional second");
            }
            frac_ns += (c - '0') * mult;
            mult /= 10;
        }
    }
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    const std::time_t secs = timegm(&tm);
    return WallTime{std::chrono::seconds{secs} + std::chrono::nanoseconds{frac_ns}};
}

Json duration_ns_json(std::uint64_t ns) {
    if (ns > 0 && ns % 1'000'000'000ull == 0) {
        return Json{{"value", std::to_string(ns / 1'000'000'000ull)}, {"unit", "s"}};
    }
    if (ns > 0 && ns % 1'000'000ull == 0) {
        return Json{{"value", std::to_string(ns / 1'000'000ull)}, {"unit", "ms"}};
    }
    if (ns > 0 && ns % 1'000ull == 0) {
        return Json{{"value", std::to_string(ns / 1'000ull)}, {"unit", "us"}};
    }
    return Json{{"value", std::to_string(ns)}, {"unit", "ns"}};
}

Json freshness_json(SnapshotFreshness state, std::optional<std::chrono::nanoseconds> age) {
    Json obj{{"state", to_string(state)}, {"age", nullptr}};
    if (age && age->count() >= 0) {
        obj["age"] = duration_ns_json(static_cast<std::uint64_t>(age->count()));
    }
    return obj;
}

Result<ApiVersion> parse_api_version_header(std::string_view header) {
    const auto dot = header.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= header.size()) {
        return Status(StatusCode::InvalidArgument, "Invalid RoDAQ-API-Version format");
    }
    const auto major_s = header.substr(0, dot);
    const auto minor_s = header.substr(dot + 1);
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    const auto [p1, e1] = std::from_chars(major_s.data(), major_s.data() + major_s.size(), major);
    const auto [p2, e2] = std::from_chars(minor_s.data(), minor_s.data() + minor_s.size(), minor);
    if (e1 != std::errc{} || p1 != major_s.data() + major_s.size() || e2 != std::errc{} ||
        p2 != minor_s.data() + minor_s.size()) {
        return Status(StatusCode::InvalidArgument, "Invalid RoDAQ-API-Version numbers");
    }
    return ApiVersion{major, minor};
}

Result<std::uint64_t> parse_decimal_header_u64(std::string_view text) {
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0')) {
        return Status(StatusCode::InvalidArgument, "Invalid decimal header");
    }
    std::uint64_t out = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return Status(StatusCode::InvalidArgument, "Decimal header out of range");
    }
    return out;
}

bool valid_idempotency_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > 128) {
        return false;
    }
    for (unsigned char c : key) {
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

Result<OperationKind> parse_lifecycle_operation_request(std::string_view body) {
    struct DuplicateTracker {
        std::vector<std::set<std::string>> stack;
        bool duplicate{false};
    } tracker;

    auto cb = [&tracker](int /*depth*/, Json::parse_event_t ev, Json& parsed) -> bool {
        if (ev == Json::parse_event_t::object_start) {
            tracker.stack.emplace_back();
        } else if (ev == Json::parse_event_t::object_end) {
            if (!tracker.stack.empty()) {
                tracker.stack.pop_back();
            }
        } else if (ev == Json::parse_event_t::key) {
            if (!tracker.stack.empty()) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!tracker.stack.back().insert(key).second) {
                    tracker.duplicate = true;
                    return false;
                }
            }
        }
        return true;
    };

    try {
        auto parsed = Json::parse(body.begin(), body.end(), cb, true, false);
        if (tracker.duplicate || parsed.is_discarded() || !parsed.is_object()) {
            return Status(StatusCode::InvalidArgument, "Invalid operation request JSON");
        }
        for (const auto& [k, _] : parsed.items()) {
            if (k != "kind" && k != "parameters") {
                return Status(StatusCode::InvalidArgument, "Unknown operation request field");
            }
        }
        if (!parsed.contains("kind") || !parsed["kind"].is_string() ||
            !parsed.contains("parameters") || !parsed["parameters"].is_object() ||
            !parsed["parameters"].empty()) {
            return Status(
                StatusCode::InvalidArgument,
                "Operation request requires kind and empty parameters object");
        }
        const auto& kind = parsed["kind"].get_ref<const std::string&>();
        if (kind == "start") {
            return OperationKind::Start;
        }
        if (kind == "pause") {
            return OperationKind::Pause;
        }
        if (kind == "resume") {
            return OperationKind::Resume;
        }
        if (kind == "stop") {
            return OperationKind::Stop;
        }
        if (kind == "reset") {
            return OperationKind::Reset;
        }
        if (kind == "retire") {
            return OperationKind::Retire;
        }
        return Status(StatusCode::InvalidArgument, "Unsupported lifecycle operation kind");
    } catch (const Json::exception&) {
        return Status(StatusCode::InvalidArgument, "Malformed operation request JSON");
    }
}

MappedError map_core_status(
    const Status& status, std::string_view default_domain, bool operation_lookup) {
    MappedError err;
    err.failure_domain = std::string(default_domain);
    err.message = status.message().empty() ? "Request failed" : std::string(status.message());
    switch (status.code()) {
        case StatusCode::InvalidArgument:
            err.http_status = drogon::k400BadRequest;
            err.code = "invalid_argument";
            err.failure_domain = "request";
            err.retryability = "never";
            break;
        case StatusCode::PermissionDenied:
            err.http_status = drogon::k403Forbidden;
            err.code = "permission_denied";
            err.failure_domain = "request";
            err.retryability = "never";
            break;
        case StatusCode::NotFound:
            err.http_status = drogon::k404NotFound;
            err.code = operation_lookup ? "operation_not_found_or_expired" : "not_found";
            err.retryability = "never";
            break;
        case StatusCode::InvalidState:
            err.http_status = drogon::k409Conflict;
            err.code = "invalid_state";
            err.retryability = "after_condition_change";
            break;
        case StatusCode::Conflict:
            err.http_status = drogon::k409Conflict;
            err.code = "conflict";
            err.retryability = "after_condition_change";
            break;
        case StatusCode::ResourceExhausted:
            err.http_status = drogon::k409Conflict;
            err.code = "resource_exhausted";
            err.retryability = "after_condition_change";
            break;
        case StatusCode::DeadlineExceeded:
            err.http_status = drogon::k504GatewayTimeout;
            err.code = "deadline_exceeded";
            err.retryability = "after_backoff";
            break;
        case StatusCode::Unavailable:
            err.http_status = drogon::k503ServiceUnavailable;
            err.code = "unavailable";
            err.retryability = "after_backoff";
            break;
        case StatusCode::DataLoss:
            err.http_status = drogon::k500InternalServerError;
            err.code = "data_loss";
            err.failure_domain = "persistence_state";
            err.data_impact = "historical_artifact_lost_or_corrupt";
            err.retryability = "manual_recovery_required";
            break;
        case StatusCode::Internal:
        case StatusCode::Ok:
        default:
            err.http_status = drogon::k500InternalServerError;
            err.code = "internal";
            err.failure_domain = "process";
            err.retryability = "never";
            break;
    }
    return err;
}

Json make_success_envelope(ApiVersion version, std::string_view request_id, Json data) {
    return Json{
        {"api_version", {{"major", version.major}, {"minor", version.minor}}},
        {"request_id", std::string(request_id)},
        {"data", std::move(data)}};
}

Json make_error_envelope(ApiVersion version, std::string_view request_id, const MappedError& err) {
    Json details = Json::array();
    for (const auto& d : err.details) {
        details.push_back(Json{{"code", d.code}, {"path", d.path}, {"message", d.message}});
    }
    return Json{
        {"api_version", {{"major", version.major}, {"minor", version.minor}}},
        {"request_id", std::string(request_id)},
        {"error",
         {{"code", err.code},
          {"message", err.message},
          {"retryability", err.retryability},
          {"failure_domain", err.failure_domain},
          {"data_impact", err.data_impact},
          {"details", std::move(details)}}}};
}

void apply_common_security_headers(
    const drogon::HttpResponsePtr& resp, ApiVersion version, std::string_view request_id,
    bool no_store) {
    resp->addHeader(
        "RoDAQ-API-Version", std::to_string(version.major) + "." + std::to_string(version.minor));
    resp->addHeader("RoDAQ-Request-ID", std::string(request_id));
    resp->addHeader(
        "Content-Security-Policy",
        "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
        "connect-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'");
    resp->addHeader("Referrer-Policy", "no-referrer");
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("X-Frame-Options", "DENY");
    if (no_store) {
        resp->addHeader("Cache-Control", "no-store");
    }
}

drogon::HttpResponsePtr make_json_response(
    drogon::HttpStatusCode status, ApiVersion version, std::string_view request_id,
    const Json& body, bool no_store) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(status);
    resp->setContentTypeCodeAndCustomString(
        drogon::CT_APPLICATION_JSON, "application/json; charset=utf-8");
    resp->setBody(body.dump());
    apply_common_security_headers(resp, version, request_id, no_store);
    return resp;
}

drogon::HttpResponsePtr make_error_response(
    ApiVersion version, std::string_view request_id, const MappedError& err) {
    return make_json_response(
        err.http_status, version, request_id, make_error_envelope(version, request_id, err), true);
}

Json to_session_dto(const SessionSnapshot& snap) {
    return Json{
        {"principal", snap.principal},
        {"scopes", snap.scopes.names()},
        {"api_version", {{"major", snap.api_version.major}, {"minor", snap.api_version.minor}}},
        {"csrf_token", snap.csrf_token}};
}

Json to_session_dto(const AuthContext& auth) {
    return Json{
        {"principal", auth.principal},
        {"scopes", auth.scopes.names()},
        {"api_version", {{"major", auth.api_version.major}, {"minor", auth.api_version.minor}}},
        {"csrf_token", auth.csrf_token}};
}

Json to_system_dto(
    ServerPhase phase, const SystemSnapshot& snap, std::size_t active_downloads,
    std::optional<std::string> web_error) {
    Json volumes = Json::array();
    for (const auto& v : snap.volumes) {
        volumes.push_back(Json{
            {"name", v.name},
            {"total_bytes", std::to_string(v.total_bytes)},
            {"free_bytes", std::to_string(v.free_bytes)},
            {"managed_bytes", std::to_string(v.managed_bytes)},
            {"quarantined_bytes", std::to_string(v.quarantined_bytes)},
            {"pressure", to_string(v.pressure)}});
    }
    Json recovery{{"complete", snap.recovery.complete}, {"reason", nullptr}, {"progress", nullptr}};
    if (web_error) {
        recovery["reason"] = *web_error;
    } else if (snap.recovery.reason) {
        recovery["reason"] = *snap.recovery.reason;
    }
    if (snap.recovery.progress) {
        recovery["progress"] = *snap.recovery.progress;
    }
    return Json{
        {"server_phase", to_string(phase)},
        {"core_ready", snap.core_ready},
        {"recovery", std::move(recovery)},
        {"resources",
         {{"cpu_cores", sanitize_rate(snap.resources.cpu_cores)},
          {"rss_bytes", std::to_string(snap.resources.rss_bytes)},
          {"threads", std::to_string(snap.resources.threads)},
          {"open_fds", std::to_string(snap.resources.open_fds)}}},
        {"volumes", std::move(volumes)},
        {"backlog",
         {{"commit_items", std::to_string(snap.backlog.commit_items)},
          {"commit_bytes", std::to_string(snap.backlog.commit_bytes)},
          {"active_downloads", std::to_string(std::max<std::uint64_t>(
                                   snap.backlog.active_downloads, active_downloads))},
          {"quarantined_items", std::to_string(snap.backlog.quarantined_items)},
          {"verification_required_items", std::to_string(snap.backlog.verification_required_items)},
          {"corrupted_committed_items", std::to_string(snap.backlog.corrupted_committed_items)}}},
        {"captured_at", format_rfc3339_utc(snap.captured_at)},
        {"snapshot_sequence", std::to_string(snap.snapshot_sequence)},
        {"freshness", freshness_json(snap.freshness, snap.age)}};
}

Json to_capabilities_dto(const SystemCapabilities& caps, const ServerConfig& config) {
    Json backends = Json::array();
    for (const auto& b : caps.backends) {
        const auto support = b.support_status == BackendSupportStatus::Required
                                 ? "required"
                                 : (b.support_status == BackendSupportStatus::QualifiedCandidate
                                        ? "qualified_candidate"
                                        : "deferred");
        const auto qual = b.qualification_status == BackendQualificationStatus::Qualified
                              ? "qualified"
                              : "not_qualified";
        backends.push_back(Json{
            {"plugin_id", b.plugin_id},
            {"plugin_version", b.plugin_version},
            {"role", b.role == BackendRole::Acquisition ? "acquisition" : "storage"},
            {"installed", b.installed},
            {"allowed", b.allowed},
            {"support_status", support},
            {"qualification_status", qual}});
    }
    Json roots = Json::array();
    for (const auto& r : caps.output_roots) {
        roots.push_back(Json{
            {"name", r},
            {"allowed", true},
            {"writable", true},
            {"free_bytes", "0"},
            {"pressure", "normal"}});
    }
    Json profiles = Json::array();
    for (const auto& p : caps.resource_profiles) {
        profiles.push_back(Json{
            {"name", p},
            {"allowed", true},
            {"max_streams_per_pipeline", "64"},
            {"max_segment_physical_bytes", "0"}});
    }
    return Json{
        {"application_version", caps.application_version},
        {"backends", std::move(backends)},
        {"output_roots", std::move(roots)},
        {"resource_profiles", std::move(profiles)},
        {"api_limits",
         {{"max_request_body_bytes", std::to_string(config.http.max_request_body_bytes)},
          {"max_page_size", std::to_string(config.http.max_page_size)},
          {"idempotency_retention",
           duration_ns_json(static_cast<std::uint64_t>(config.idempotency_retention.count()))},
          {"recommended_polling_floor", duration_ns_json(static_cast<std::uint64_t>(
                                            config.http.recommended_polling_floor.count()))}}}};
}

Json to_task_dto(
    const TaskSnapshot& snap, const std::optional<std::string>& name, SnapshotFreshness freshness) {
    Json obj{
        {"task_id", rodaq::to_string(snap.task_id)},
        {"name", name && !name->empty() ? Json(*name) : Json(nullptr)},
        {"config_version", snap.active_config_version},
        {"desired_state", to_string(snap.desired_state)},
        {"execution_condition", to_string(snap.execution_condition)},
        {"control_generation", std::to_string(snap.control_generation)},
        {"current_operation_id", snap.current_operation
                                     ? Json(rodaq::to_string(snap.current_operation->operation_id))
                                     : Json(nullptr)},
        {"data_impact", to_string(snap.data_impact)},
        {"captured_at", format_rfc3339_utc(snap.captured_at)},
        {"snapshot_sequence", std::to_string(snap.snapshot_sequence)},
        {"freshness", freshness_json(freshness, std::chrono::nanoseconds{0})}};
    return obj;
}

Json to_task_summary_dto(const TaskSummary& summary) {
    Json obj = to_task_dto(summary.task, std::nullopt, summary.freshness);
    obj["data_impact"] = to_string(summary.data_impact);
    obj["runtime_summary"] = Json{
        {"accepted_records_per_second", summary.accepted_records_per_second
                                            ? sanitize_rate(*summary.accepted_records_per_second)
                                            : 0.0},
        {"commit_lag",
         summary.commit_lag && summary.commit_lag->count() >= 0
             ? duration_ns_json(static_cast<std::uint64_t>(summary.commit_lag->count()))
             : Json(nullptr)},
        {"receiving_streams", "0"},
        {"total_streams", "0"}};
    return obj;
}

Json to_task_page_dto(const TaskPage& page) {
    Json items = Json::array();
    for (const auto& item : page.items) {
        items.push_back(to_task_summary_dto(item));
    }
    return Json{
        {"items", std::move(items)},
        {"has_more", page.has_more},
        {"next_cursor", page.has_more ? page.next_cursor : std::string{}}};
}

Json to_operation_dto(TaskId task_id, const OperationSnapshot& snap) {
    return Json{
        {"task_id", rodaq::to_string(task_id)},
        {"operation_id", rodaq::to_string(snap.operation_id)},
        {"kind", to_string(snap.kind)},
        {"state", to_string(snap.state)},
        {"control_generation", std::to_string(snap.control_generation)},
        {"accepted_at", format_rfc3339_utc(snap.accepted_at)},
        {"terminal_at",
         snap.terminal_at ? Json(format_rfc3339_utc(*snap.terminal_at)) : Json(nullptr)},
        {"retryability", to_string(snap.retryability)},
        {"failure_domain", to_string(snap.failure_domain)},
        {"data_impact", to_string(snap.data_impact)},
        {"message", snap.diagnostic.empty() ? Json(nullptr) : Json(snap.diagnostic)}};
}

Json to_operation_page_dto(TaskId task_id, const OperationPage& page) {
    Json items = Json::array();
    for (const auto& item : page.items) {
        items.push_back(to_operation_dto(task_id, item));
    }
    return Json{
        {"items", std::move(items)},
        {"has_more", page.has_more},
        {"next_cursor", page.has_more ? page.next_cursor : std::string{}},
        {"history_truncated", page.history_truncated},
        {"oldest_retained_control_generation",
         page.oldest_retained_control_generation
             ? Json(std::to_string(*page.oldest_retained_control_generation))
             : Json(nullptr)},
        {"oldest_retained_at", page.oldest_retained_at
                                   ? Json(format_rfc3339_utc(*page.oldest_retained_at))
                                   : Json(nullptr)}};
}

Json to_task_runtime_dto(const TaskRuntimeSnapshot& snap) {
    const auto window_json =
        snap.window && snap.window->count() >= 0
            ? duration_ns_json(static_cast<std::uint64_t>(snap.window->count()))
            : duration_ns_json(1'000'000'000ull);
    Json pipelines = Json::array();
    for (const auto& p : snap.pipelines) {
        std::uint64_t p_recv_rec = 0;
        std::uint64_t p_recv_bytes = 0;
        double p_recv_rps = 0.0;
        double p_recv_bps = 0.0;
        std::uint64_t p_acc = 0;
        std::uint64_t p_rej = 0;
        Json streams = Json::array();
        for (const auto& s : p.streams) {
            std::uint64_t s_rej = 0;
            Json rej_reasons = Json::array();
            if (s.facts) {
                p_recv_rec += s.facts->received_records;
                p_recv_bytes += s.facts->received_logical_bytes;
                p_recv_rps += s.facts->received_records_per_second
                                  ? sanitize_rate(*s.facts->received_records_per_second)
                                  : 0.0;
                p_recv_bps += s.facts->received_logical_bytes_per_second
                                  ? sanitize_rate(*s.facts->received_logical_bytes_per_second)
                                  : 0.0;
                p_acc += s.facts->accepted_records;
                for (const auto& r : s.facts->rejections) {
                    s_rej += r.records;
                    rej_reasons.push_back(Json{
                        {"reason", to_string(r.reason)}, {"records", std::to_string(r.records)}});
                }
                p_rej += s_rej;
            }
            streams.push_back(Json{
                {"stream_id", std::to_string(s.stream_id)},
                {"stream_key", s.stream_key},
                {"name", s.name.empty() ? Json(nullptr) : Json(s.name)},
                {"entry_ready", s.facts ? s.facts->entry_ready : false},
                {"source_ready", s.facts ? s.facts->source_ready : false},
                {"receiving", s.facts && s.facts->receiving ? *s.facts->receiving : false},
                {"last_receive_at", s.facts && s.facts->last_receive_at
                                        ? Json(format_rfc3339_utc(*s.facts->last_receive_at))
                                        : Json(nullptr)},
                {"window", window_json},
                {"received",
                 {{"records", std::to_string(s.facts ? s.facts->received_records : 0)},
                  {"logical_bytes", std::to_string(s.facts ? s.facts->received_logical_bytes : 0)},
                  {"records_per_second", s.facts && s.facts->received_records_per_second
                                             ? sanitize_rate(*s.facts->received_records_per_second)
                                             : 0.0},
                  {"logical_bytes_per_second",
                   s.facts && s.facts->received_logical_bytes_per_second
                       ? sanitize_rate(*s.facts->received_logical_bytes_per_second)
                       : 0.0}}},
                {"admission",
                 {{"accepted_records", std::to_string(s.facts ? s.facts->accepted_records : 0)},
                  {"rejected_records", std::to_string(s_rej)},
                  {"reject_reasons", std::move(rej_reasons)}}}});
        }

        const bool open = p.facts && p.facts->admission_open;
        const std::string cond =
            !p.facts ? "idle"
                     : (p.facts->acquisition_terminal_failure ||
                                p.facts->writer_failure != StatusCode::Ok
                            ? "failed"
                            : (open ? "active" : (p.facts->quiescent ? "idle" : "draining")));
        pipelines.push_back(Json{
            {"pipeline_id", std::to_string(p.pipeline_id)},
            {"required", p.required},
            {"instance_id",
             p.facts && !p.facts->instance_id.empty() ? Json(p.facts->instance_id) : Json(nullptr)},
            {"condition", cond},
            {"gate_state", open ? "open" : "closed"},
            {"received",
             {{"records", std::to_string(p_recv_rec)},
              {"logical_bytes", std::to_string(p_recv_bytes)},
              {"records_per_second", p_recv_rps},
              {"logical_bytes_per_second", p_recv_bps}}},
            {"admission",
             {{"accepted_records", std::to_string(p.facts ? p.facts->accepted_records : p_acc)},
              {"rejected_records", std::to_string(p_rej)},
              {"reject_reasons", Json::array()}}},
            {"queue",
             {{"records", std::to_string(p.facts ? p.facts->queued_records : 0)},
              {"bytes", std::to_string(p.facts ? p.facts->queued_logical_bytes : 0)},
              {"high_watermark_records", std::to_string(p.facts ? p.facts->queued_records : 0)},
              {"high_watermark_bytes", std::to_string(p.facts ? p.facts->queued_logical_bytes : 0)},
              {"oldest_age", nullptr}}},
            {"writer",
             {{"logical_bytes_per_second", 0.0},
              {"pending_records", std::to_string(p.facts ? p.facts->unsettled_records : 0)},
              {"last_progress_at", nullptr}}},
            {"storage",
             {{"physical_bytes_per_second", 0.0},
              {"pending_bytes", "0"},
              {"last_progress_at", nullptr}}},
            {"commit",
             {{"lag", nullptr},
              {"durable_bytes_per_second", 0.0},
              {"pending_segments", std::to_string(p.facts ? p.facts->commit_items : 0)},
              {"last_committed_artifact_id", nullptr},
              {"last_progress_at", nullptr}}},
            {"streams", std::move(streams)}});
    }

    return Json{
        {"task_id", rodaq::to_string(snap.task_id)},
        {"captured_at", format_rfc3339_utc(snap.captured_at)},
        {"snapshot_sequence", std::to_string(snap.snapshot_sequence)},
        {"freshness", freshness_json(snap.freshness, snap.age)},
        {"pipelines", std::move(pipelines)}};
}

Json to_artifact_summary_dto(const ArtifactSummary& s) {
    return Json{
        {"artifact_id", rodaq::to_string(s.artifact_id)},
        {"task_id", rodaq::to_string(s.task_id)},
        {"pipeline_id", std::to_string(s.pipeline_id)},
        {"segment_id", std::to_string(s.segment_id)},
        {"committed_at", format_rfc3339_utc(s.committed_at)},
        {"recording_range",
         {{"begin", duration_ns_json(s.recording_range.begin_ns)},
          {"end", duration_ns_json(s.recording_range.end_ns)}}},
        {"format", s.format},
        {"logical_bytes", std::to_string(s.logical_bytes)},
        {"physical_bytes", std::to_string(s.physical_bytes)},
        {"data_impact", to_string(s.data_impact)}};
}

Json to_artifact_page_dto(const ArtifactPage& page) {
    Json items = Json::array();
    for (const auto& item : page.items) {
        items.push_back(to_artifact_summary_dto(item));
    }
    return Json{
        {"items", std::move(items)},
        {"has_more", page.has_more},
        {"next_cursor", page.has_more ? page.next_cursor : std::string{}}};
}

Json to_artifact_detail_dto(const ArtifactInfo& info) {
    Json obj = to_artifact_summary_dto(info.summary);
    const auto aid = rodaq::to_string(info.summary.artifact_id);
    Json files = Json::array();
    for (const auto& f : info.files) {
        const auto fid = rodaq::to_string(f.file_id);
        files.push_back(Json{
            {"file_id", fid},
            {"display_name", f.display_name},
            {"size", std::to_string(f.size)},
            {"media_type", f.media_type},
            {"etag", f.etag},
            {"download_url", "/api/artifacts/" + aid + "/files/" + fid}});
    }
    std::uint64_t gap_count = 0;
    Json gaps = Json::array();
    for (const auto& c : info.coverage) {
        if (c.known_failed > 0) {
            ++gap_count;
            if (gaps.size() < 64) {
                gaps.push_back(Json{
                    {"kind", "storage_failure"},
                    {"recording_begin", nullptr},
                    {"recording_end", nullptr},
                    {"message", "Stream " + std::to_string(c.stream_id) +
                                    " known_failed=" + std::to_string(c.known_failed)}});
            }
        }
        if (c.unknown > 0) {
            ++gap_count;
            if (gaps.size() < 64) {
                gaps.push_back(Json{
                    {"kind", "persistence_unknown"},
                    {"recording_begin", nullptr},
                    {"recording_end", nullptr},
                    {"message", "Stream " + std::to_string(c.stream_id) +
                                    " unknown=" + std::to_string(c.unknown)}});
            }
        }
    }
    obj["coverage"] = Json{
        {"complete", gap_count == 0 && info.summary.data_impact == DataImpact::None},
        {"gap_count", std::to_string(gap_count)},
        {"gaps", std::move(gaps)}};
    obj["files"] = std::move(files);
    return obj;
}

}  // namespace rodaq::server
