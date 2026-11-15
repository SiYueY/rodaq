#include "control_record.hpp"

#include "enum_codec.hpp"
#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"
#include "../time/wall_time.hpp"

#include <algorithm>
#include <initializer_list>
#include <new>
#include <utility>

namespace rodaq::task {
namespace {

using persistence::Json;
constexpr std::string_view kDomain = "rodaq.task-control.v1";

persistence::JsonLimits limits(const HistoryPolicy& policy) {
    return {control_record_max_bytes(policy), 6, 256, 20, policy.max_entries,
            32 + policy.max_entries * 20};
}

struct InvalidRecord {
    Status status;
};

void require(bool valid) {
    if (!valid) {
        throw InvalidRecord{Status(StatusCode::InvalidArgument, {})};
    }
}

template <typename Value>
Value take(Result<Value> result) {
    if (!result.ok()) {
        throw InvalidRecord{std::move(result).status()};
    }
    return std::move(result).value();
}

void fields(const Json& object, std::initializer_list<const char*> names) {
    require(object.is_object() && object.size() == names.size());
    for (const auto* name : names) {
        require(object.contains(name));
    }
}

const std::string& text(const Json& object, const char* field) {
    const auto& value = object.at(field);
    require(value.is_string());
    return value.get_ref<const std::string&>();
}

std::uint64_t number(const Json& object, const char* field) {
    return take(persistence::unsigned_decimal(text(object, field)));
}

Json operation_document(const StoredOperation& operation) {
    const auto& snapshot = operation.snapshot;
    return {
        {"operation_id", to_string(snapshot.operation_id)},
        {"request_key", operation.request.request_key},
        {"request_digest", operation.request_digest},
        {"operation_kind", enum_text(snapshot.kind)},
        {"target_control_generation", std::to_string(snapshot.control_generation)},
        {"accepted_at", take(time::format_wall_time(snapshot.accepted_at))},
        {"state", enum_text(snapshot.state)},
        {"terminal_result",
         terminal(snapshot.state) ? Json(enum_text(snapshot.state)) : Json(nullptr)},
        {"terminal_at", snapshot.terminal_at
                            ? Json(take(time::format_wall_time(*snapshot.terminal_at)))
                            : Json(nullptr)},
        {"retryability", enum_text(snapshot.retryability)},
        {"failure_domain", enum_text(snapshot.failure_domain)},
        {"data_impact", enum_text(snapshot.data_impact)},
        {"status_code", std::to_string(static_cast<std::int32_t>(snapshot.status_code))},
        {"diagnostic", snapshot.diagnostic}};
}

StoredOperation read_operation(const Json& document) {
    fields(
        document,
        {"operation_id", "request_key", "request_digest", "operation_kind",
         "target_control_generation", "accepted_at", "state", "terminal_result", "terminal_at",
         "retryability", "failure_domain", "data_impact", "status_code", "diagnostic"});
    StoredOperation operation;
    operation.request.request_key = text(document, "request_key");
    operation.request_digest = text(document, "request_digest");
    auto& snapshot = operation.snapshot;
    snapshot.operation_id = take(parse_operation_id(text(document, "operation_id")));
    snapshot.kind = take(parse_enum<OperationKind>(text(document, "operation_kind")));
    snapshot.control_generation = number(document, "target_control_generation");
    snapshot.accepted_at = take(time::parse_wall_time(text(document, "accepted_at")));
    snapshot.state = take(parse_enum<OperationState>(text(document, "state")));
    if (!document.at("terminal_at").is_null()) {
        snapshot.terminal_at = take(time::parse_wall_time(text(document, "terminal_at")));
    }
    if (terminal(snapshot.state)) {
        require(text(document, "terminal_result") == enum_text(snapshot.state));
    } else {
        require(document.at("terminal_result").is_null());
    }
    snapshot.retryability = take(parse_enum<Retryability>(text(document, "retryability")));
    snapshot.failure_domain = take(parse_enum<FailureDomain>(text(document, "failure_domain")));
    snapshot.data_impact = take(parse_enum<DataImpact>(text(document, "data_impact")));
    const auto code = number(document, "status_code");
    require(code <= static_cast<std::uint64_t>(StatusCode::Internal));
    snapshot.status_code = static_cast<StatusCode>(code);
    snapshot.diagnostic = text(document, "diagnostic");
    return operation;
}

bool nonzero(TaskId id) noexcept {
    return std::any_of(
        id.value.begin(), id.value.end(), [](std::uint8_t byte) { return byte != 0; });
}

}  // namespace

std::size_t control_record_max_bytes(const HistoryPolicy& policy) noexcept {
    // Per-entry bound includes worst-case JSON escapes for diagnostics and keys.
    // Validate HistoryPolicy before using this bound for allocation or I/O.
    return policy.max_entries * 4096 + 2048;
}

Result<void> validate_control_record(const TaskControlRecord& record, const HistoryPolicy& policy) {
    auto history = validate_history(record.operation_history, policy);
    if (!history.ok()) {
        return std::move(history).status();
    }
    if (!nonzero(record.task_id) || !persistence::valid_digest(record.active_config_version) ||
        record.control_generation == 0 || record.record_revision < record.control_generation ||
        enum_text(record.desired_state).empty()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const StoredOperation* current = nullptr;
    const StoredOperation* resetting = nullptr;
    for (const auto& operation : record.operation_history.entries) {
        if (operation.snapshot.control_generation > record.control_generation) {
            return Status(StatusCode::InvalidArgument, {});
        }
        if (record.current_operation_id &&
            operation.snapshot.operation_id == *record.current_operation_id) {
            current = &operation;
        }
        if (!terminal(operation.snapshot.state)) {
            if (!record.current_operation_id ||
                !(operation.snapshot.operation_id == *record.current_operation_id)) {
                return Status(StatusCode::InvalidArgument, {});
            }
            if (operation.snapshot.kind == OperationKind::Reset) {
                resetting = &operation;
            }
        }
    }
    if (record.current_operation_id &&
        (current == nullptr || current->snapshot.control_generation != record.control_generation)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (resetting != nullptr) {
        if (!record.reset_intent || record.desired_state != DesiredState::Stopped ||
            !(record.reset_intent->operation_id == resetting->snapshot.operation_id) ||
            record.reset_intent->reset_generation != record.reset_generation ||
            record.reset_generation == 0 ||
            (record.reset_intent->target_control_generation == 0 ||
             record.reset_intent->target_control_generation >= record.control_generation)) {
            return Status(StatusCode::InvalidArgument, {});
        }
    } else if (record.reset_intent) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return {};
}

Result<std::string> encode_control_record(
    const TaskControlRecord& record, const HistoryPolicy& policy) {
    auto valid = validate_control_record(record, policy);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    try {
        Json history = Json::array();
        for (const auto& operation : record.operation_history.entries) {
            history.push_back(operation_document(operation));
        }
        Json reset = nullptr;
        if (record.reset_intent) {
            const auto& intent = *record.reset_intent;
            reset = {
                {"operation_id", to_string(intent.operation_id)},
                {"reset_generation", std::to_string(intent.reset_generation)},
                {"target_control_generation", std::to_string(intent.target_control_generation)},
                {"scope", "all_task_artifacts"}};
        }
        const Json document{
            {"format_version", "1.0"},
            {"task_id", to_string(record.task_id)},
            {"active_config_version", record.active_config_version},
            {"desired_state", enum_text(record.desired_state)},
            {"control_generation", std::to_string(record.control_generation)},
            {"reset_generation", std::to_string(record.reset_generation)},
            {"record_revision", std::to_string(record.record_revision)},
            {"current_operation_id", record.current_operation_id
                                         ? Json(to_string(*record.current_operation_id))
                                         : Json(nullptr)},
            {"operation_history", std::move(history)},
            {"history_truncated", record.operation_history.history_truncated},
            {"reset_intent", std::move(reset)}};
        return persistence::encode_integrity(document, kDomain, limits(policy));
    } catch (InvalidRecord& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<TaskControlRecord> decode_control_record(
    std::string_view bytes, const HistoryPolicy& policy) {
    auto policy_valid = validate_history({}, policy);
    if (!policy_valid.ok()) {
        return std::move(policy_valid).status();
    }
    auto parsed = persistence::parse_json(bytes, limits(policy));
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : std::move(parsed).status();
    }
    try {
        const auto& document = parsed.value();
        fields(
            document,
            {"format_version", "task_id", "active_config_version", "desired_state",
             "control_generation", "reset_generation", "record_revision", "current_operation_id",
             "operation_history", "history_truncated", "reset_intent", "content_digest"});
        if (text(document, "format_version") != "1.0") {
            return Status(StatusCode::Unavailable, {});
        }
        TaskControlRecord record;
        record.task_id = take(parse_task_id(text(document, "task_id")));
        record.active_config_version = text(document, "active_config_version");
        record.desired_state = take(parse_enum<DesiredState>(text(document, "desired_state")));
        record.control_generation = number(document, "control_generation");
        record.reset_generation = number(document, "reset_generation");
        record.record_revision = number(document, "record_revision");
        if (!document.at("current_operation_id").is_null()) {
            record.current_operation_id =
                take(parse_operation_id(text(document, "current_operation_id")));
        }
        require(document.at("operation_history").is_array());
        require(document.at("history_truncated").is_boolean());
        record.operation_history.history_truncated = document.at("history_truncated").get<bool>();
        for (const auto& entry : document.at("operation_history")) {
            record.operation_history.entries.push_back(read_operation(entry));
        }
        const auto& reset = document.at("reset_intent");
        if (!reset.is_null()) {
            fields(
                reset, {"operation_id", "reset_generation", "target_control_generation", "scope"});
            require(text(reset, "scope") == "all_task_artifacts");
            record.reset_intent = ResetIntent{
                take(parse_operation_id(text(reset, "operation_id"))),
                number(reset, "reset_generation"), number(reset, "target_control_generation")};
        }
        auto valid = validate_control_record(record, policy);
        if (!valid.ok()) {
            return valid.status().code() == StatusCode::InvalidArgument
                       ? Status(StatusCode::DataLoss, {})
                       : std::move(valid).status();
        }
        auto verified = persistence::verify_integrity(document, kDomain, limits(policy));
        if (!verified.ok()) {
            return std::move(verified).status();
        }
        return record;
    } catch (InvalidRecord& failure) {
        return failure.status.code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::task
