#include "ledger_record.hpp"

#include <array>
#include <new>
#include <utility>

#include <rodaq/plugin/abi.hpp>

#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"
#include "../uuid.hpp"

namespace rodaq::artifact {
namespace {

constexpr auto kDomain = "rodaq.artifact-ledger.v1";
constexpr auto kStates = std::array{
    std::pair{LedgerState::Committing, std::string_view{"committing"}},
    std::pair{LedgerState::Committed, std::string_view{"committed"}},
    std::pair{LedgerState::Deleting, std::string_view{"deleting"}},
    std::pair{LedgerState::Deleted, std::string_view{"deleted"}}};
using persistence::Json;

persistence::JsonLimits limits() { return {4096, 2, 256, 16, 0, 17}; }

struct InvalidLedger {
    Status status;
};

void require(bool valid) {
    if (!valid) {
        throw InvalidLedger{Status(StatusCode::InvalidArgument, {})};
    }
}

template <typename Value>
Value take(Result<Value> result) {
    if (!result.ok()) {
        throw InvalidLedger{std::move(result).status()};
    }
    return std::move(result).value();
}

const std::string& text(const Json& document, const char* field) {
    const auto& value = document.at(field);
    require(value.is_string());
    return value.get_ref<const std::string&>();
}

std::string_view state_text(LedgerState state) noexcept {
    for (const auto& entry : kStates) {
        if (entry.first == state) {
            return entry.second;
        }
    }
    return {};
}

LedgerState parse_state(std::string_view state) {
    for (const auto& entry : kStates) {
        if (entry.second == state) {
            return entry.first;
        }
    }
    throw InvalidLedger{Status(StatusCode::InvalidArgument, {})};
}

void validate(const LedgerRecord& record) {
    require(
        record.segment_id != 0 && uuid::nonzero(record.task_id.value) && record.pipeline_id != 0 &&
        uuid::nonzero(record.volume_id.value) && record.control_generation != 0);
    require(!state_text(record.state).empty() && persistence::valid_digest(record.manifest_digest));
    require(
        record.managed_relative_path == ".rodaq/artifacts/" + std::to_string(record.segment_id));
    (void)take(time::format_wall_time(record.commit_started_at));
    require(record.committed_at.has_value() == (record.state != LedgerState::Committing));
    const bool deleting =
        record.state == LedgerState::Deleting || record.state == LedgerState::Deleted;
    require(
        record.delete_generation.has_value() == deleting &&
        record.delete_reason.has_value() == deleting);
    require(record.deleted_at.has_value() == (record.state == LedgerState::Deleted));
    if (record.committed_at) {
        (void)take(time::format_wall_time(*record.committed_at));
    }
    if (record.deleted_at) {
        (void)take(time::format_wall_time(*record.deleted_at));
    }
    if (deleting) {
        const auto& reason = *record.delete_reason;
        require(
            *record.delete_generation != 0 && !reason.empty() && reason.size() <= 128 &&
            reason.find('\0') == std::string::npos &&
            plugin::abi::utf8_prefix(reason, reason.size()) == reason.size());
    }
    // These are factual wall times. Clock jumps may make later lifecycle facts
    // numerically earlier; generation/state, not wall ordering, owns causality.
}

Json document(const LedgerRecord& record) {
    return {
        {"format_version", "1.0"},
        {"segment_id", std::to_string(record.segment_id)},
        {"state", state_text(record.state)},
        {"task_id", to_string(record.task_id)},
        {"pipeline_id", std::to_string(record.pipeline_id)},
        {"volume_id", to_string(record.volume_id)},
        {"managed_relative_path", record.managed_relative_path},
        {"manifest_digest", record.manifest_digest},
        {"control_generation", std::to_string(record.control_generation)},
        {"reset_generation_at_commit", std::to_string(record.reset_generation_at_commit)},
        {"commit_started_at", take(time::format_wall_time(record.commit_started_at))},
        {"committed_at", record.committed_at
                             ? Json(take(time::format_wall_time(*record.committed_at)))
                             : Json(nullptr)},
        {"delete_generation", record.delete_generation
                                  ? Json(std::to_string(*record.delete_generation))
                                  : Json(nullptr)},
        {"delete_reason", record.delete_reason ? Json(*record.delete_reason) : Json(nullptr)},
        {"deleted_at", record.deleted_at ? Json(take(time::format_wall_time(*record.deleted_at)))
                                         : Json(nullptr)}};
}

}  // namespace

Result<void> validate_ledger_record(const LedgerRecord& record) {
    try {
        validate(record);
        return {};
    } catch (InvalidLedger& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<std::string> encode_ledger_record(const LedgerRecord& record) {
    try {
        validate(record);
        return persistence::encode_integrity(document(record), kDomain, limits());
    } catch (InvalidLedger& failure) {
        return std::move(failure.status);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<LedgerRecord> decode_ledger_record(std::string_view bytes) {
    try {
        const auto parsed = take(persistence::parse_json(bytes, limits()));
        require(parsed.is_object() && parsed.size() == 16);
        for (const auto* field :
             {"format_version", "segment_id", "state", "task_id", "pipeline_id", "volume_id",
              "managed_relative_path", "manifest_digest", "control_generation",
              "reset_generation_at_commit", "commit_started_at", "committed_at",
              "delete_generation", "delete_reason", "deleted_at", "content_digest"}) {
            require(parsed.contains(field));
        }
        if (text(parsed, "format_version") != "1.0") {
            return Status(StatusCode::Unavailable, {});
        }
        LedgerRecord record;
        record.segment_id = take(persistence::unsigned_decimal(text(parsed, "segment_id")));
        record.state = parse_state(text(parsed, "state"));
        record.task_id = take(parse_task_id(text(parsed, "task_id")));
        record.pipeline_id = take(persistence::unsigned_decimal(text(parsed, "pipeline_id")));
        record.volume_id = take(parse_volume_id(text(parsed, "volume_id")));
        record.managed_relative_path = text(parsed, "managed_relative_path");
        record.manifest_digest = text(parsed, "manifest_digest");
        record.control_generation =
            take(persistence::unsigned_decimal(text(parsed, "control_generation")));
        record.reset_generation_at_commit =
            take(persistence::unsigned_decimal(text(parsed, "reset_generation_at_commit")));
        record.commit_started_at = take(time::parse_wall_time(text(parsed, "commit_started_at")));
        if (!parsed.at("committed_at").is_null()) {
            record.committed_at = take(time::parse_wall_time(text(parsed, "committed_at")));
        }
        if (!parsed.at("delete_generation").is_null()) {
            record.delete_generation =
                take(persistence::unsigned_decimal(text(parsed, "delete_generation")));
        }
        if (!parsed.at("delete_reason").is_null()) {
            record.delete_reason = text(parsed, "delete_reason");
        }
        if (!parsed.at("deleted_at").is_null()) {
            record.deleted_at = take(time::parse_wall_time(text(parsed, "deleted_at")));
        }
        validate(record);
        take(persistence::verify_integrity(parsed, kDomain, limits()));
        return record;
    } catch (InvalidLedger& failure) {
        return failure.status.code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : std::move(failure.status);
    } catch (const Json::exception&) {
        return Status(StatusCode::DataLoss, {});
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> validate_ledger_transition(const LedgerRecord& before, const LedgerRecord& after) {
    auto old_valid = validate_ledger_record(before);
    if (!old_valid.ok()) {
        return old_valid.status();
    }
    auto new_valid = validate_ledger_record(after);
    if (!new_valid.ok()) {
        return new_valid.status();
    }
    if (before.segment_id != after.segment_id || !(before.task_id == after.task_id) ||
        before.pipeline_id != after.pipeline_id || !(before.volume_id == after.volume_id) ||
        before.managed_relative_path != after.managed_relative_path ||
        before.manifest_digest != after.manifest_digest ||
        before.control_generation != after.control_generation ||
        before.reset_generation_at_commit != after.reset_generation_at_commit ||
        before.commit_started_at != after.commit_started_at ||
        (before.committed_at && before.committed_at != after.committed_at) ||
        (before.delete_generation && before.delete_generation != after.delete_generation) ||
        (before.delete_reason && before.delete_reason != after.delete_reason) ||
        (before.deleted_at && before.deleted_at != after.deleted_at)) {
        return Status(StatusCode::Conflict, {});
    }
    if (before.state == after.state ||
        (before.state == LedgerState::Committing && after.state == LedgerState::Committed) ||
        (before.state == LedgerState::Committed && after.state == LedgerState::Deleting) ||
        (before.state == LedgerState::Deleting && after.state == LedgerState::Deleted)) {
        return {};
    }
    return Status(StatusCode::Conflict, {});
}

}  // namespace rodaq::artifact
