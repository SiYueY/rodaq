#include "recovery_record.hpp"

#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"
#include "../uuid.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>

namespace rodaq::task {
namespace {

using persistence::Json;
constexpr std::string_view kDomain = "rodaq.task-recovery.v1";

struct InvalidRecord {};

void require(bool condition) {
    if (!condition) {
        throw InvalidRecord{};
    }
}

template <typename T>
T take(Result<T> result) {
    require(result.ok());
    return std::move(result).value();
}

persistence::JsonLimits json_limits(const RecoveryLimits& limits) {
    return {
        limits.max_bytes,
        7,
        256,
        16,
        std::max({limits.max_pipelines, limits.max_cleanup_intents, limits.max_reset_volumes}),
        128 + limits.max_pipelines * 64 + limits.max_cleanup_intents * 8 +
            limits.max_reset_volumes * 8};
}

void fields(const Json& value, std::initializer_list<const char*> names) {
    require(value.is_object() && value.size() == names.size());
    for (const auto* name : names) {
        require(value.contains(name));
    }
}

const std::string& text(const Json& value, const char* field) {
    const auto& result = value.at(field);
    require(result.is_string());
    return result.get_ref<const std::string&>();
}

std::uint64_t number(const Json& value, const char* field) {
    return take(persistence::unsigned_decimal(text(value, field)));
}

bool boolean(const Json& value, const char* field) {
    require(value.at(field).is_boolean());
    return value.at(field).get<bool>();
}

std::string_view phase_text(RunPhase phase) noexcept {
    switch (phase) {
        case RunPhase::Prepared:
            return "Prepared";
        case RunPhase::OpenAuthorized:
            return "OpenAuthorized";
        case RunPhase::Ended:
            return "Ended";
    }
    return {};
}

RunPhase read_phase(std::string_view value) {
    if (value == "Prepared") {
        return RunPhase::Prepared;
    }
    if (value == "OpenAuthorized") {
        return RunPhase::OpenAuthorized;
    }
    require(value == "Ended");
    return RunPhase::Ended;
}

void validate_boundary(const RecoveryBoundary& boundary) {
    auto boot = uuid::parse(boundary.boot_uuid);
    require(boot.ok() && uuid::nonzero(boot.value()));
}

bool same_boundary(const RecoveryBoundary& a, const RecoveryBoundary& b) noexcept {
    return a.boot_uuid == b.boot_uuid && a.monotonic_ns == b.monotonic_ns;
}

bool same_optional_boundary(
    const std::optional<RecoveryBoundary>& a, const std::optional<RecoveryBoundary>& b) noexcept {
    return a.has_value() == b.has_value() && (!a || same_boundary(*a, *b));
}

Json boundary_document(const RecoveryBoundary& boundary) {
    return {
        {"boot_uuid", boundary.boot_uuid}, {"monotonic_ns", std::to_string(boundary.monotonic_ns)}};
}

Json optional_boundary(const std::optional<RecoveryBoundary>& boundary) {
    return boundary ? boundary_document(*boundary) : Json(nullptr);
}

RecoveryBoundary read_boundary(const Json& value) {
    fields(value, {"boot_uuid", "monotonic_ns"});
    return {text(value, "boot_uuid"), number(value, "monotonic_ns")};
}

std::optional<RecoveryBoundary> read_optional_boundary(const Json& value) {
    return value.is_null() ? std::nullopt : std::optional<RecoveryBoundary>(read_boundary(value));
}

void validate_pipeline(const AuthorizedPipeline& value) {
    require(
        value.pipeline_id != 0 && pipeline::valid_instance_id(value.instance_id) &&
        persistence::valid_digest(value.effective_config_id));
    validate_boundary(value.prepared_boundary);
    if (value.ended_boundary) {
        validate_boundary(*value.ended_boundary);
        require(
            value.ended_boundary->boot_uuid != value.prepared_boundary.boot_uuid ||
            value.ended_boundary->monotonic_ns >= value.prepared_boundary.monotonic_ns);
    }
}

bool same_pipeline(const AuthorizedPipeline& a, const AuthorizedPipeline& b) noexcept {
    return a.pipeline_id == b.pipeline_id && a.instance_id == b.instance_id &&
           a.effective_config_id == b.effective_config_id &&
           same_boundary(a.prepared_boundary, b.prepared_boundary);
}

bool progresses(
    const AuthorizedPipeline& a, const AuthorizedPipeline& b, bool higher_control) noexcept {
    return same_pipeline(a, b) && (!a.opened_observed || b.opened_observed) &&
           (a.continuation_permission == b.continuation_permission ||
            (higher_control && a.continuation_permission && !b.continuation_permission)) &&
           (!a.ended_boundary || same_optional_boundary(a.ended_boundary, b.ended_boundary));
}

Json pipeline_document(const AuthorizedPipeline& value) {
    return {
        {"pipeline_id", std::to_string(value.pipeline_id)},
        {"instance_id", pipeline::to_string(value.instance_id)},
        {"effective_config_id", value.effective_config_id},
        {"prepared_boundary", boundary_document(value.prepared_boundary)},
        {"continuation_permission", value.continuation_permission},
        {"opened_observed", value.opened_observed},
        {"ended_boundary", optional_boundary(value.ended_boundary)}};
}

AuthorizedPipeline read_pipeline(const Json& value) {
    fields(
        value, {"pipeline_id", "instance_id", "effective_config_id", "prepared_boundary",
                "continuation_permission", "opened_observed", "ended_boundary"});
    return {
        number(value, "pipeline_id"),
        take(pipeline::parse_instance_id(text(value, "instance_id"))),
        text(value, "effective_config_id"),
        read_boundary(value.at("prepared_boundary")),
        boolean(value, "continuation_permission"),
        boolean(value, "opened_observed"),
        read_optional_boundary(value.at("ended_boundary"))};
}

template <typename T, typename Predicate>
bool unique_before(const std::vector<T>& items, std::size_t index, Predicate duplicate) {
    return std::none_of(
        items.begin(), items.begin() + static_cast<std::ptrdiff_t>(index), duplicate);
}

}  // namespace

bool valid_recovery_limits(const RecoveryLimits& limits) noexcept {
    return limits.max_bytes >= 1024 && limits.max_bytes <= 16 * 1024 * 1024 &&
           limits.max_pipelines > 0 && limits.max_pipelines <= 4096 &&
           limits.max_cleanup_intents <= 4096 && limits.max_reset_volumes <= 4096;
}

Result<void> validate_recovery_record(
    const TaskRecoveryRecord& record, const RecoveryLimits& limits) {
    try {
        require(
            valid_recovery_limits(limits) && uuid::nonzero(record.task_id.value) &&
            record.record_revision != 0 && record.observed_control_generation != 0 &&
            persistence::valid_digest(record.active_config_version));
        require(
            record.pipelines.size() <= limits.max_pipelines &&
            record.cleanup_intents.size() <= limits.max_cleanup_intents);
        if (record.run_operation) {
            const auto& run = *record.run_operation;
            require(
                uuid::nonzero(run.operation_id.value) && run.control_generation != 0 &&
                run.control_generation <= record.observed_control_generation &&
                !phase_text(run.phase).empty() && !run.authorized_pipelines.empty() &&
                run.authorized_pipelines.size() <= limits.max_pipelines);
            require(run.phase != RunPhase::Prepared || !run.authorization_boundary);
            require(run.phase != RunPhase::OpenAuthorized || run.authorization_boundary);
            if (run.authorization_boundary) {
                validate_boundary(*run.authorization_boundary);
            }
            for (std::size_t i = 0; i < run.authorized_pipelines.size(); ++i) {
                const auto& entry = run.authorized_pipelines[i];
                validate_pipeline(entry);
                require(unique_before(run.authorized_pipelines, i, [&](const auto& old) {
                    return old.pipeline_id == entry.pipeline_id ||
                           old.instance_id == entry.instance_id;
                }));
                require(!entry.opened_observed || run.authorization_boundary);
                require(run.phase != RunPhase::Ended || entry.ended_boundary);
                if (run.authorization_boundary) {
                    require(
                        run.authorization_boundary->boot_uuid ==
                            entry.prepared_boundary.boot_uuid &&
                        run.authorization_boundary->monotonic_ns >=
                            entry.prepared_boundary.monotonic_ns);
                    require(
                        !entry.ended_boundary ||
                        entry.ended_boundary->boot_uuid != run.authorization_boundary->boot_uuid ||
                        entry.ended_boundary->monotonic_ns >=
                            run.authorization_boundary->monotonic_ns);
                }
            }
        }
        for (std::size_t i = 0; i < record.pipelines.size(); ++i) {
            const auto& entry = record.pipelines[i];
            require(
                entry.control_generation != 0 &&
                entry.control_generation <= record.observed_control_generation);
            validate_pipeline(entry.instance);
            require(unique_before(record.pipelines, i, [&](const auto& old) {
                return old.instance.pipeline_id == entry.instance.pipeline_id ||
                       old.instance.instance_id == entry.instance.instance_id;
            }));
        }
        for (std::size_t i = 0; i < record.cleanup_intents.size(); ++i) {
            const auto& entry = record.cleanup_intents[i];
            require(
                entry.pipeline_id != 0 && pipeline::valid_instance_id(entry.instance_id) &&
                entry.control_generation != 0 &&
                entry.control_generation <= record.observed_control_generation);
            require(unique_before(record.cleanup_intents, i, [&](const auto& old) {
                return old.instance_id == entry.instance_id;
            }));
        }
        if (record.reset_progress) {
            const auto& reset = *record.reset_progress;
            require(
                uuid::nonzero(reset.operation_id.value) && reset.reset_generation != 0 &&
                reset.volumes.size() <= limits.max_reset_volumes);
            for (std::size_t i = 0; i < reset.volumes.size(); ++i) {
                const auto& entry = reset.volumes[i];
                require(
                    uuid::nonzero(entry.volume_id.value) &&
                    (!entry.ledger_settled || entry.files_removed));
                require(unique_before(reset.volumes, i, [&](const auto& old) {
                    return old.volume_id == entry.volume_id;
                }));
            }
        }
        return {};
    } catch (const InvalidRecord&) {
        return Status(StatusCode::InvalidArgument, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> encode_recovery_record(
    const TaskRecoveryRecord& record, const RecoveryLimits& limits) {
    auto valid = validate_recovery_record(record, limits);
    if (!valid.ok()) {
        return valid.status();
    }
    try {
        Json run = nullptr;
        if (record.run_operation) {
            const auto& value = *record.run_operation;
            Json entries = Json::array();
            for (const auto& entry : value.authorized_pipelines) {
                entries.push_back(pipeline_document(entry));
            }
            run = {
                {"operation_id", to_string(value.operation_id)},
                {"control_generation", std::to_string(value.control_generation)},
                {"phase", phase_text(value.phase)},
                {"authorized_pipelines", std::move(entries)},
                {"authorization_boundary", optional_boundary(value.authorization_boundary)}};
        }
        Json pipelines = Json::array();
        for (const auto& entry : record.pipelines) {
            pipelines.push_back(
                {{"control_generation", std::to_string(entry.control_generation)},
                 {"instance", pipeline_document(entry.instance)}});
        }
        Json cleanup = Json::array();
        for (const auto& entry : record.cleanup_intents) {
            cleanup.push_back(
                {{"pipeline_id", std::to_string(entry.pipeline_id)},
                 {"instance_id", pipeline::to_string(entry.instance_id)},
                 {"control_generation", std::to_string(entry.control_generation)}});
        }
        Json reset = nullptr;
        if (record.reset_progress) {
            const auto& value = *record.reset_progress;
            Json volumes = Json::array();
            for (const auto& volume : value.volumes) {
                volumes.push_back(
                    {{"volume_id", artifact::to_string(volume.volume_id)},
                     {"files_removed", volume.files_removed},
                     {"ledger_settled", volume.ledger_settled},
                     {"catalog_settled", volume.catalog_settled}});
            }
            reset = {
                {"operation_id", to_string(value.operation_id)},
                {"reset_generation", std::to_string(value.reset_generation)},
                {"volumes", std::move(volumes)}};
        }
        return persistence::encode_integrity(
            {{"format_version", "1.0"},
             {"task_id", to_string(record.task_id)},
             {"record_revision", std::to_string(record.record_revision)},
             {"observed_control_generation", std::to_string(record.observed_control_generation)},
             {"active_config_version", record.active_config_version},
             {"run_operation", std::move(run)},
             {"pipelines", std::move(pipelines)},
             {"reset_progress", std::move(reset)},
             {"cleanup_intents", std::move(cleanup)}},
            kDomain, json_limits(limits));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskRecoveryRecord> decode_recovery_record(
    std::string_view bytes, const RecoveryLimits& limits) {
    if (!valid_recovery_limits(limits)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto parsed = persistence::parse_json(bytes, json_limits(limits));
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::ResourceExhausted
                   ? parsed.status()
                   : Status(StatusCode::DataLoss, {});
    }
    try {
        const auto& doc = parsed.value();
        fields(
            doc, {"format_version", "task_id", "record_revision", "observed_control_generation",
                  "active_config_version", "run_operation", "pipelines", "reset_progress",
                  "cleanup_intents", "content_digest"});
        require(text(doc, "format_version") == "1.0");
        TaskRecoveryRecord record;
        record.task_id = take(parse_task_id(text(doc, "task_id")));
        record.record_revision = number(doc, "record_revision");
        record.observed_control_generation = number(doc, "observed_control_generation");
        record.active_config_version = text(doc, "active_config_version");
        const auto& run = doc.at("run_operation");
        if (!run.is_null()) {
            fields(
                run, {"operation_id", "control_generation", "phase", "authorized_pipelines",
                      "authorization_boundary"});
            RunRecovery value;
            value.operation_id = take(parse_operation_id(text(run, "operation_id")));
            value.control_generation = number(run, "control_generation");
            value.phase = read_phase(text(run, "phase"));
            value.authorization_boundary = read_optional_boundary(run.at("authorization_boundary"));
            require(run.at("authorized_pipelines").is_array());
            for (const auto& entry : run.at("authorized_pipelines")) {
                value.authorized_pipelines.push_back(read_pipeline(entry));
            }
            record.run_operation = std::move(value);
        }
        require(doc.at("pipelines").is_array() && doc.at("cleanup_intents").is_array());
        for (const auto& entry : doc.at("pipelines")) {
            fields(entry, {"control_generation", "instance"});
            record.pipelines.push_back(
                {number(entry, "control_generation"), read_pipeline(entry.at("instance"))});
        }
        for (const auto& entry : doc.at("cleanup_intents")) {
            fields(entry, {"pipeline_id", "instance_id", "control_generation"});
            record.cleanup_intents.push_back(
                {number(entry, "pipeline_id"),
                 take(pipeline::parse_instance_id(text(entry, "instance_id"))),
                 number(entry, "control_generation")});
        }
        const auto& reset = doc.at("reset_progress");
        if (!reset.is_null()) {
            fields(reset, {"operation_id", "reset_generation", "volumes"});
            ResetProgress value;
            value.operation_id = take(parse_operation_id(text(reset, "operation_id")));
            value.reset_generation = number(reset, "reset_generation");
            require(reset.at("volumes").is_array());
            for (const auto& volume : reset.at("volumes")) {
                fields(volume, {"volume_id", "files_removed", "ledger_settled", "catalog_settled"});
                value.volumes.push_back(
                    {take(artifact::parse_volume_id(text(volume, "volume_id"))),
                     boolean(volume, "files_removed"), boolean(volume, "ledger_settled"),
                     boolean(volume, "catalog_settled")});
            }
            record.reset_progress = std::move(value);
        }
        require(validate_recovery_record(record, limits).ok());
        auto integrity = persistence::verify_integrity(doc, kDomain, json_limits(limits));
        if (!integrity.ok()) {
            return integrity.status();
        }
        return record;
    } catch (const InvalidRecord&) {
        return Status(StatusCode::DataLoss, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> recovery_successor(
    const TaskRecoveryRecord& previous, const TaskRecoveryRecord& next) {
    if (!(previous.task_id == next.task_id) ||
        previous.record_revision == std::numeric_limits<std::uint64_t>::max() ||
        next.record_revision != previous.record_revision + 1 ||
        next.observed_control_generation < previous.observed_control_generation ||
        (next.observed_control_generation == previous.observed_control_generation &&
         next.active_config_version != previous.active_config_version)) {
        return Status(StatusCode::Conflict, {});
    }
    const bool higher_control =
        next.observed_control_generation > previous.observed_control_generation;
    if (previous.run_operation) {
        const auto& old = *previous.run_operation;
        const auto* run = next.run_operation ? &*next.run_operation : nullptr;
        if (run && run->operation_id == old.operation_id) {
            if (run->control_generation != old.control_generation || run->phase < old.phase ||
                run->authorized_pipelines.size() != old.authorized_pipelines.size() ||
                ((old.authorization_boundary || old.phase == RunPhase::Ended) &&
                 !same_optional_boundary(
                     old.authorization_boundary, run->authorization_boundary))) {
                return Status(StatusCode::Conflict, {});
            }
            for (const auto& entry : old.authorized_pipelines) {
                const auto found = std::find_if(
                    run->authorized_pipelines.begin(), run->authorized_pipelines.end(),
                    [&](const auto& value) { return value.pipeline_id == entry.pipeline_id; });
                if (found == run->authorized_pipelines.end() ||
                    !progresses(entry, *found, higher_control)) {
                    return Status(StatusCode::Conflict, {});
                }
            }
        } else {
            if (run && run->control_generation <= old.control_generation) {
                return Status(StatusCode::Conflict, {});
            }
            if (run && std::any_of(
                           run->authorized_pipelines.begin(), run->authorized_pipelines.end(),
                           [&](const auto& entry) {
                               return std::any_of(
                                   old.authorized_pipelines.begin(), old.authorized_pipelines.end(),
                                   [&](const auto& value) {
                                       return entry.instance_id == value.instance_id;
                                   });
                           })) {
                return Status(StatusCode::Conflict, {});
            }
            for (const auto& entry : old.authorized_pipelines) {
                if (entry.ended_boundary) {
                    continue;
                }
                const bool retained = std::any_of(
                    next.pipelines.begin(), next.pipelines.end(), [&](const auto& value) {
                        return value.control_generation == old.control_generation &&
                               progresses(entry, value.instance, higher_control);
                    });
                const bool cleanup = std::any_of(
                    next.cleanup_intents.begin(), next.cleanup_intents.end(),
                    [&](const auto& value) {
                        return value.control_generation == old.control_generation &&
                               value.pipeline_id == entry.pipeline_id &&
                               value.instance_id == entry.instance_id;
                    });
                if (!retained && !cleanup) {
                    return Status(StatusCode::Conflict, {});
                }
            }
        }
    }
    for (const auto& entry : next.pipelines) {
        const auto old = std::find_if(
            previous.pipelines.begin(), previous.pipelines.end(), [&](const auto& value) {
                return value.instance.instance_id == entry.instance.instance_id;
            });
        if (old != previous.pipelines.end() &&
            (old->control_generation != entry.control_generation ||
             !progresses(old->instance, entry.instance, higher_control))) {
            return Status(StatusCode::Conflict, {});
        }
    }
    if (previous.reset_progress && next.reset_progress) {
        const auto& old = *previous.reset_progress;
        const auto& reset = *next.reset_progress;
        if (reset.reset_generation < old.reset_generation) {
            return Status(StatusCode::Conflict, {});
        }
        if (reset.reset_generation == old.reset_generation) {
            if (!(reset.operation_id == old.operation_id) ||
                reset.volumes.size() != old.volumes.size()) {
                return Status(StatusCode::Conflict, {});
            }
            for (const auto& volume : old.volumes) {
                const auto found = std::find_if(
                    reset.volumes.begin(), reset.volumes.end(),
                    [&](const auto& value) { return value.volume_id == volume.volume_id; });
                if (found == reset.volumes.end() ||
                    (volume.files_removed && !found->files_removed) ||
                    (volume.ledger_settled && !found->ledger_settled) ||
                    (volume.catalog_settled && !found->catalog_settled)) {
                    return Status(StatusCode::Conflict, {});
                }
            }
        }
    }
    return {};
}

}  // namespace rodaq::task
