#include "dto.hpp"

#include <algorithm>
#include <cmath>

namespace rodaq::server {
namespace {

double safe_rate(std::optional<double> rate) noexcept {
    if (!rate || !std::isfinite(*rate) || *rate < 0.0) {
        return 0.0;
    }
    return *rate;
}

std::int64_t to_non_negative_ms(std::chrono::nanoseconds d) noexcept {
    if (d <= std::chrono::nanoseconds::zero()) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

std::uint64_t ns_to_ms_u64(std::uint64_t ns) noexcept { return ns / 1000000ULL; }

std::string escape_json_pointer(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (const char ch : token) {
        if (ch == '~') {
            out += "~0";
        } else if (ch == '/') {
            out += "~1";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::string_view pipeline_condition_wire(const PipelineRuntimeSnapshot& pipe) noexcept {
    if (!pipe.facts) {
        return "idle";
    }
    const auto& f = *pipe.facts;
    if (f.prepare_status && *f.prepare_status != StatusCode::Ok) {
        return "failed";
    }
    if (f.writer_failure != StatusCode::Ok || f.acquisition_terminal_failure) {
        return "failed";
    }
    if (f.admission_open) {
        return "active";
    }
    if (!f.quiescent) {
        return "draining";
    }
    return "idle";
}

}  // namespace

std::string_view to_wire_string(VolumePressure pressure) noexcept {
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

std::string_view to_wire_string(BackendRole role) noexcept {
    switch (role) {
        case BackendRole::Acquisition:
            return "acquisition";
        case BackendRole::Storage:
            return "storage";
    }
    return "acquisition";
}

std::string_view to_wire_string(SupportStatus status) noexcept {
    switch (status) {
        case SupportStatus::Required:
            return "required";
        case SupportStatus::QualifiedCandidate:
            return "qualified_candidate";
        case SupportStatus::Deferred:
            return "deferred";
    }
    return "deferred";
}

std::string_view to_wire_string(QualificationStatus status) noexcept {
    switch (status) {
        case QualificationStatus::Qualified:
            return "qualified";
        case QualificationStatus::NotQualified:
            return "not_qualified";
    }
    return "not_qualified";
}

std::string_view to_wire_string(RecordRejectionReason reason) noexcept {
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

nlohmann::json serialize_freshness(SnapshotFreshness freshness, std::chrono::nanoseconds age) {
    if (freshness == SnapshotFreshness::NotObserved) {
        return nlohmann::json{{"state", "unknown"}, {"age_ms", nullptr}};
    }
    return nlohmann::json{
        {"state", freshness == SnapshotFreshness::Fresh ? "fresh" : "stale"},
        {"age_ms", to_non_negative_ms(age)}};
}

nlohmann::json serialize_system(
    ServerPhase phase, const SystemSnapshot& snapshot, std::uint64_t active_downloads,
    const std::optional<std::string>& deployment_error) {
    nlohmann::json volumes = nlohmann::json::array();
    for (const auto& vol : snapshot.volumes) {
        volumes.push_back(
            {{"name", vol.name},
             {"total_bytes", vol.total_bytes},
             {"free_bytes", vol.free_bytes},
             {"pressure", to_wire_string(vol.pressure)}});
    }
    const double cpu =
        std::isfinite(snapshot.resources.cpu_cores) && snapshot.resources.cpu_cores >= 0.0
            ? snapshot.resources.cpu_cores
            : 0.0;
    nlohmann::json out{
        {"server_phase", to_wire_string(phase)},
        {"core_ready", snapshot.core_ready},
        {"resources",
         {{"cpu_cores", cpu},
          {"rss_bytes", snapshot.resources.rss_bytes},
          {"threads", snapshot.resources.threads},
          {"open_fds", snapshot.resources.open_fds}}},
        {"volumes", std::move(volumes)},
        {"backlog",
         {{"commit_items", snapshot.backlog.commit_items},
          {"commit_bytes", snapshot.backlog.commit_bytes},
          {"active_downloads", active_downloads}}},
        {"captured_at", format_rfc3339_utc(snapshot.captured_at)},
        {"snapshot_sequence", std::to_string(snapshot.snapshot_sequence)},
        {"freshness", serialize_freshness(snapshot.freshness, snapshot.age)}};
    if (deployment_error) {
        out["deployment_error"] = *deployment_error;
    }
    return out;
}

nlohmann::json serialize_capabilities(
    const SystemCapabilities& caps, const SystemSnapshot& snapshot, const ServerConfig& config) {
    nlohmann::json backends = nlohmann::json::array();
    for (const auto& b : caps.backends) {
        if (!b.role) {
            continue;
        }
        backends.push_back(
            {{"plugin_id", b.plugin_id},
             {"plugin_version", b.plugin_version},
             {"role", to_wire_string(*b.role)},
             {"installed", b.installed},
             {"allowed", b.allowed},
             {"support_status", to_wire_string(b.support_status)},
             {"qualification_status", to_wire_string(b.qualification_status)}});
    }
    nlohmann::json output_roots = nlohmann::json::array();
    for (const auto& name : caps.output_roots) {
        std::uint64_t free_bytes = 0;
        VolumePressure pressure = VolumePressure::Protected;
        for (const auto& vol : snapshot.volumes) {
            if (vol.name == name) {
                free_bytes = vol.free_bytes;
                pressure = vol.pressure;
                break;
            }
        }
        output_roots.push_back(
            {{"name", name},
             {"allowed", true},
             {"writable", pressure != VolumePressure::Protected},
             {"free_bytes", free_bytes},
             {"pressure", to_wire_string(pressure)}});
    }
    nlohmann::json profiles = nlohmann::json::array();
    for (const auto& p : caps.resource_profiles) {
        profiles.push_back(
            {{"name", p.name},
             {"allowed", true},
             {"max_streams_per_pipeline", p.max_streams_per_pipeline},
             {"max_segment_physical_bytes", p.max_segment_physical_bytes}});
    }
    const auto effective_page =
        std::min<std::uint32_t>(config.http.max_page_size, caps.limits.max_page_size);
    const auto effective_poll = std::max(
        std::chrono::duration_cast<std::chrono::nanoseconds>(config.http.recommended_polling_floor),
        caps.limits.recommended_polling_floor);
    return nlohmann::json{
        {"application_version", caps.application_version},
        {"backends", std::move(backends)},
        {"output_roots", std::move(output_roots)},
        {"resource_profiles", std::move(profiles)},
        {"api_limits",
         {{"max_request_body_bytes", config.http.max_request_body_bytes},
          {"max_page_size", effective_page},
          {"recommended_polling_floor_ms", to_non_negative_ms(effective_poll)}}}};
}

nlohmann::json serialize_task_data(const TaskSnapshot& snapshot) {
    nlohmann::json name_val =
        snapshot.name.empty() ? nlohmann::json(nullptr) : nlohmann::json(snapshot.name);
    nlohmann::json cur_op =
        snapshot.current_operation
            ? nlohmann::json(to_string(snapshot.current_operation->operation_id))
            : nlohmann::json(nullptr);
    return nlohmann::json{
        {"task_id", to_string(snapshot.task_id)},
        {"name", std::move(name_val)},
        {"config_version", snapshot.active_config_version},
        {"desired_state", to_wire_string(snapshot.desired_state)},
        {"execution_condition", to_wire_string(snapshot.execution_condition)},
        {"control_generation", std::to_string(snapshot.control_generation)},
        {"current_operation_id", std::move(cur_op)},
        {"data_impact", to_wire_string(snapshot.data_impact)},
        {"captured_at", format_rfc3339_utc(snapshot.captured_at)},
        {"snapshot_sequence", std::to_string(snapshot.snapshot_sequence)},
        {"freshness", serialize_freshness(snapshot.freshness, snapshot.age)}};
}

nlohmann::json serialize_task_create(TaskId task_id, const std::string& config_version) {
    return nlohmann::json{{"task_id", to_string(task_id)}, {"config_version", config_version}};
}

nlohmann::json serialize_task_page(const TaskPage& page) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : page.items) {
        nlohmann::json name_val =
            item.task.name.empty() ? nlohmann::json(nullptr) : nlohmann::json(item.task.name);
        nlohmann::json cur_op =
            item.task.current_operation
                ? nlohmann::json(to_string(item.task.current_operation->operation_id))
                : nlohmann::json(nullptr);
        nlohmann::json commit_lag_ms = item.commit_lag
                                           ? nlohmann::json(to_non_negative_ms(*item.commit_lag))
                                           : nlohmann::json(nullptr);
        items.push_back(
            {{"task_id", to_string(item.task.task_id)},
             {"name", std::move(name_val)},
             {"config_version", item.task.active_config_version},
             {"desired_state", to_wire_string(item.task.desired_state)},
             {"execution_condition", to_wire_string(item.task.execution_condition)},
             {"control_generation", std::to_string(item.task.control_generation)},
             {"current_operation_id", std::move(cur_op)},
             {"data_impact", to_wire_string(item.data_impact)},
             {"runtime_summary",
              {{"accepted_records_per_second", safe_rate(item.accepted_records_per_second)},
               {"commit_lag_ms", std::move(commit_lag_ms)},
               {"receiving_streams", item.receiving_streams},
               {"total_streams", item.total_streams}}},
             {"captured_at", format_rfc3339_utc(item.task.captured_at)},
             {"freshness", serialize_freshness(item.freshness, item.task.age)}});
    }
    return nlohmann::json{
        {"items", std::move(items)},
        {"has_more", page.has_more},
        {"next_cursor", page.has_more ? page.next_cursor : std::string{}}};
}

nlohmann::json serialize_mutation_accepted(TaskId task_id, OperationId operation_id) {
    return nlohmann::json{
        {"task_id", to_string(task_id)},
        {"operation_id", to_string(operation_id)},
        {"state", "accepted"}};
}

nlohmann::json serialize_operation(TaskId task_id, const OperationSnapshot& op) {
    nlohmann::json terminal_at = op.terminal_at
                                     ? nlohmann::json(format_rfc3339_utc(*op.terminal_at))
                                     : nlohmann::json(nullptr);
    nlohmann::json message = op.diagnostic.empty()
                                 ? nlohmann::json(nullptr)
                                 : nlohmann::json(sanitize_bounded_message(op.diagnostic));
    return nlohmann::json{
        {"task_id", to_string(task_id)},
        {"operation_id", to_string(op.operation_id)},
        {"kind", to_wire_string(op.kind)},
        {"state", to_wire_string(op.state)},
        {"control_generation", std::to_string(op.control_generation)},
        {"accepted_at", format_rfc3339_utc(op.accepted_at)},
        {"terminal_at", std::move(terminal_at)},
        {"retryability", to_wire_string(op.retryability)},
        {"failure_domain", to_wire_string(op.failure_domain)},
        {"data_impact", to_wire_string(op.data_impact)},
        {"message", std::move(message)}};
}

nlohmann::json serialize_operation_page(TaskId task_id, const OperationPage& page) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& op : page.items) {
        items.push_back(serialize_operation(task_id, op));
    }
    return nlohmann::json{
        {"items", std::move(items)},
        {"has_more", page.has_more},
        {"next_cursor", page.has_more ? page.next_cursor : std::string{}},
        {"history_truncated", page.history_truncated}};
}

nlohmann::json serialize_task_runtime(const TaskRuntimeSnapshot& runtime) {
    const auto window_ms =
        to_non_negative_ms(runtime.window ? *runtime.window : std::chrono::seconds{1});
    const auto captured_str = format_rfc3339_utc(runtime.captured_at);
    const auto freshness_json = serialize_freshness(runtime.freshness, runtime.age);

    nlohmann::json pipelines = nlohmann::json::array();
    for (const auto& pipe : runtime.pipelines) {
        std::uint64_t total_received_records = 0;
        std::uint64_t total_received_bytes = 0;
        double total_rec_rate = 0.0;
        double total_byte_rate = 0.0;
        std::uint64_t total_rejected = 0;

        nlohmann::json streams = nlohmann::json::array();
        for (const auto& s : pipe.streams) {
            nlohmann::json reject_reasons = nlohmann::json::array();
            std::uint64_t stream_rejected = 0;
            if (s.facts) {
                total_received_records += s.facts->received_records;
                total_received_bytes += s.facts->received_logical_bytes;
                total_rec_rate += safe_rate(s.facts->received_records_per_second);
                total_byte_rate += safe_rate(s.facts->received_logical_bytes_per_second);
                for (const auto& r : s.facts->rejections) {
                    stream_rejected += r.records;
                    reject_reasons.push_back(
                        {{"reason", to_wire_string(r.reason)}, {"records", r.records}});
                }
                total_rejected += stream_rejected;
            }
            nlohmann::json name_val =
                s.name.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.name);
            nlohmann::json last_recv =
                (s.facts && s.facts->last_receive_at)
                    ? nlohmann::json(format_rfc3339_utc(*s.facts->last_receive_at))
                    : nlohmann::json(nullptr);
            streams.push_back(
                {{"stream_id", std::to_string(s.stream_id)},
                 {"stream_key", s.stream_key},
                 {"name", std::move(name_val)},
                 {"entry_ready", s.facts ? s.facts->entry_ready : false},
                 {"source_ready", s.facts ? s.facts->source_ready : false},
                 {"receiving", s.facts ? s.facts->receiving.value_or(false) : false},
                 {"last_receive_at", std::move(last_recv)},
                 {"window_ms", window_ms},
                 {"received",
                  {{"records", s.facts ? s.facts->received_records : 0u},
                   {"logical_bytes", s.facts ? s.facts->received_logical_bytes : 0u},
                   {"records_per_second",
                    safe_rate(s.facts ? s.facts->received_records_per_second : std::nullopt)},
                   {"logical_bytes_per_second",
                    safe_rate(
                        s.facts ? s.facts->received_logical_bytes_per_second : std::nullopt)}}},
                 {"admission",
                  {{"accepted_records", s.facts ? s.facts->accepted_records : 0u},
                   {"rejected_records", stream_rejected},
                   {"reject_reasons", std::move(reject_reasons)}}},
                 {"captured_at", captured_str},
                 {"freshness", freshness_json}});
        }

        const auto accepted = pipe.facts ? pipe.facts->accepted_records : 0u;
        const auto queued_rec = pipe.facts ? pipe.facts->queued_records : 0u;
        const auto queued_bytes = pipe.facts ? pipe.facts->queued_logical_bytes : 0u;
        const auto unsettled = pipe.facts ? pipe.facts->unsettled_records : 0u;
        const auto commit_items = pipe.facts ? pipe.facts->commit_items : 0u;
        nlohmann::json instance_id = (pipe.facts && !pipe.facts->instance_id.empty())
                                         ? nlohmann::json(pipe.facts->instance_id)
                                         : nlohmann::json(nullptr);
        pipelines.push_back(
            {{"pipeline_id", std::to_string(pipe.pipeline_id)},
             {"required", pipe.required},
             {"instance_id", std::move(instance_id)},
             {"condition", pipeline_condition_wire(pipe)},
             {"gate_state", (pipe.facts && pipe.facts->admission_open) ? "open" : "closed"},
             {"received",
              {{"records", total_received_records},
               {"logical_bytes", total_received_bytes},
               {"records_per_second", total_rec_rate},
               {"logical_bytes_per_second", total_byte_rate}}},
             {"admission",
              {{"accepted_records", accepted},
               {"rejected_records", total_rejected},
               {"reject_reasons", nlohmann::json::array()}}},
             {"queue",
              {{"records", queued_rec},
               {"bytes", queued_bytes},
               {"high_watermark_records", queued_rec},
               {"high_watermark_bytes", queued_bytes},
               {"oldest_age_ms", nullptr}}},
             {"writer",
              {{"logical_bytes_per_second", total_byte_rate},
               {"pending_records", unsettled},
               {"last_progress_at", nullptr}}},
             {"storage",
              {{"physical_bytes_per_second", 0.0},
               {"pending_bytes", queued_bytes},
               {"last_progress_at", nullptr}}},
             {"commit",
              {{"lag_ms", nullptr},
               {"durable_bytes_per_second", 0.0},
               {"pending_segments", commit_items},
               {"last_committed_artifact_id", nullptr},
               {"last_progress_at", nullptr}}},
             {"streams", std::move(streams)}});
    }
    return nlohmann::json{
        {"task_id", to_string(runtime.task_id)},
        {"captured_at", captured_str},
        {"snapshot_sequence", std::to_string(runtime.snapshot_sequence)},
        {"freshness", freshness_json},
        {"pipelines", std::move(pipelines)}};
}

nlohmann::json serialize_artifact_summary(const ArtifactSummary& summary) {
    return nlohmann::json{
        {"artifact_id", to_string(summary.artifact_id)},
        {"task_id", to_string(summary.task_id)},
        {"pipeline_id", std::to_string(summary.pipeline_id)},
        {"segment_id", std::to_string(summary.segment_id)},
        {"committed_at", format_rfc3339_utc(summary.committed_at)},
        {"recording_range",
         {{"begin_ms", ns_to_ms_u64(summary.recording_range.begin_ns)},
          {"end_ms", ns_to_ms_u64(summary.recording_range.end_ns)}}},
        {"format", summary.format},
        {"logical_bytes", summary.logical_bytes},
        {"physical_bytes", summary.physical_bytes},
        {"data_impact", to_wire_string(summary.data_impact)}};
}

nlohmann::json serialize_artifact_detail(const ArtifactInfo& info) {
    auto doc = serialize_artifact_summary(info.summary);
    const std::string art_id = to_string(info.summary.artifact_id);
    nlohmann::json files = nlohmann::json::array();
    for (const auto& f : info.files) {
        const std::string fid = to_string(f.file_id);
        files.push_back(
            {{"file_id", fid},
             {"display_name", f.display_name},
             {"size", f.size},
             {"media_type", f.media_type},
             {"etag", f.etag},
             {"download_url", "/api/artifacts/" + art_id + "/files/" + fid}});
    }
    std::uint64_t gap_count = 0;
    nlohmann::json gaps = nlohmann::json::array();
    for (const auto& cov : info.coverage) {
        if (cov.known_failed > 0) {
            ++gap_count;
            if (gaps.size() < 32) {
                gaps.push_back(
                    {{"kind", "storage_failure"},
                     {"recording_begin_ms", nullptr},
                     {"recording_end_ms", nullptr},
                     {"message", "accepted records failed during storage write"}});
            }
        }
        if (cov.unknown > 0) {
            ++gap_count;
            if (gaps.size() < 32) {
                gaps.push_back(
                    {{"kind", "persistence_unknown"},
                     {"recording_begin_ms", nullptr},
                     {"recording_end_ms", nullptr},
                     {"message", "accepted records ended with unknown persistence"}});
            }
        }
    }
    doc["coverage"] = {
        {"complete", gap_count == 0 && info.summary.data_impact == DataImpact::None},
        {"gap_count", gap_count},
        {"gaps", std::move(gaps)}};
    doc["files"] = std::move(files);
    return doc;
}

nlohmann::json serialize_artifact_page(const ArtifactPage& page) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : page.items) {
        items.push_back(serialize_artifact_summary(item));
    }
    return nlohmann::json{
        {"items", std::move(items)},
        {"has_more", page.has_more},
        {"next_cursor", page.has_more ? page.next_cursor : std::string{}}};
}

Result<OperationRequestDto> parse_operation_request(
    std::string_view body, const JsonParseLimits& limits, ApiError& error_out) {
    const auto parsed = parse_bounded_json(body, limits);
    if (!parsed.ok) {
        error_out = make_request_error(
            400, "invalid_json", parsed.error.message,
            {{parsed.error.code, parsed.error.path, parsed.error.message}});
        return Status(StatusCode::InvalidArgument, parsed.error.message);
    }
    const auto& obj = parsed.value;
    if (!obj.is_object()) {
        error_out = make_request_error(
            400, "invalid_request", "operation body must be a JSON object",
            {{"invalid_type", "", "expected JSON object"}});
        return Status(StatusCode::InvalidArgument, "expected JSON object");
    }
    std::vector<ErrorDetail> details;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (it.key() != "kind" && it.key() != "parameters") {
            details.push_back(
                {"unknown_field", "/" + escape_json_pointer(it.key()), "unknown field"});
        }
    }
    if (!obj.contains("kind")) {
        details.push_back({"required", "/kind", "kind is required"});
    } else if (!obj["kind"].is_string()) {
        details.push_back({"invalid_type", "/kind", "kind must be a string"});
    }
    if (!obj.contains("parameters")) {
        details.push_back({"required", "/parameters", "parameters is required"});
    } else if (!obj["parameters"].is_object()) {
        details.push_back({"invalid_type", "/parameters", "parameters must be an empty object"});
    } else if (!obj["parameters"].empty()) {
        for (auto it = obj["parameters"].begin(); it != obj["parameters"].end(); ++it) {
            details.push_back(
                {"unknown_field", "/parameters/" + escape_json_pointer(it.key()),
                 "V1 lifecycle parameters must be empty"});
        }
    }
    OperationRequestDto dto;
    if (obj.contains("kind") && obj["kind"].is_string()) {
        const auto& kind = obj["kind"].get_ref<const std::string&>();
        if (kind == "start") {
            dto.kind = OperationKind::Start;
        } else if (kind == "pause") {
            dto.kind = OperationKind::Pause;
        } else if (kind == "resume") {
            dto.kind = OperationKind::Resume;
        } else if (kind == "stop") {
            dto.kind = OperationKind::Stop;
        } else if (kind == "reset") {
            dto.kind = OperationKind::Reset;
        } else if (kind == "retire") {
            dto.kind = OperationKind::Retire;
        } else {
            details.push_back(
                {"invalid_enum", "/kind",
                 "kind must be one of start, pause, resume, stop, reset, retire"});
        }
    }
    if (!details.empty()) {
        error_out = make_request_error(
            400, "invalid_request", "invalid lifecycle operation request", std::move(details));
        return Status(StatusCode::InvalidArgument, "invalid lifecycle operation request");
    }
    return dto;
}

}  // namespace rodaq::server
