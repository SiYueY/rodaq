#include "operation_history.hpp"

#include "identity.hpp"
#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"

#include <algorithm>
#include <new>
#include <set>
#include <utility>

#include <rodaq/plugin/abi.hpp>

namespace rodaq::task {
namespace {

template <typename Id>
bool valid_id(const Id& id) noexcept {
    return std::any_of(
        id.value.begin(), id.value.end(), [](std::uint8_t byte) { return byte != 0; });
}

bool emergency(OperationKind kind) noexcept {
    return kind == OperationKind::Pause || kind == OperationKind::Stop ||
           kind == OperationKind::Reset;
}

bool valid_kind(OperationKind kind) noexcept {
    switch (kind) {
        case OperationKind::Configure:
        case OperationKind::Start:
        case OperationKind::Pause:
        case OperationKind::Resume:
        case OperationKind::Stop:
        case OperationKind::Reset:
        case OperationKind::Retire:
            return true;
    }
    return false;
}

bool expired(
    const OperationSnapshot& operation, WallTime now, std::chrono::nanoseconds window) noexcept {
    if (!terminal(operation.state) || !operation.terminal_at || now < *operation.terminal_at) {
        return false;
    }
    // Unsigned subtraction represents the full signed-clock span without
    // overflowing when the two wall times straddle the signed epoch boundary.
    const auto elapsed =
        static_cast<std::uint64_t>(now.time_since_epoch().count()) -
        static_cast<std::uint64_t>(operation.terminal_at->time_since_epoch().count());
    return elapsed >= static_cast<std::uint64_t>(window.count());
}

struct Cursor {
    ControlGeneration generation{0};
    OperationId operation_id;
};

Result<Cursor> parse_cursor(TaskId task_id, std::string_view cursor) {
    const std::string prefix = "operations:v1:" + to_string(task_id) + ":";
    if (cursor.size() > 128 || cursor.substr(0, prefix.size()) != prefix) {
        return Status(StatusCode::InvalidArgument, {});
    }
    cursor.remove_prefix(prefix.size());
    const auto separator = cursor.find(':');
    if (separator == std::string_view::npos) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto generation = persistence::unsigned_decimal(cursor.substr(0, separator));
    auto operation = parse_operation_id(cursor.substr(separator + 1));
    if (!generation.ok() || generation.value() == 0 || !operation.ok()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return Cursor{generation.value(), operation.value()};
}

}  // namespace

bool terminal(OperationState state) noexcept {
    return state == OperationState::Succeeded || state == OperationState::PartiallySucceeded ||
           state == OperationState::Failed || state == OperationState::Superseded;
}

Result<void> validate_operation(const StoredOperation& operation) {
    const auto& snapshot = operation.snapshot;
    const auto state = snapshot.state;
    if (!valid_id(snapshot.operation_id) || snapshot.control_generation == 0 ||
        !validate_request_identity(operation.request).ok() ||
        !persistence::valid_digest(operation.request_digest) || !valid_kind(snapshot.kind) ||
        (state != OperationState::Accepted && state != OperationState::InProgress &&
         !terminal(state)) ||
        terminal(state) != snapshot.terminal_at.has_value() || snapshot.diagnostic.size() > 256 ||
        plugin::abi::utf8_prefix(snapshot.diagnostic, snapshot.diagnostic.size()) !=
            snapshot.diagnostic.size() ||
        static_cast<std::int32_t>(snapshot.status_code) < 0 ||
        static_cast<std::int32_t>(snapshot.status_code) >
            static_cast<std::int32_t>(StatusCode::Internal)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    // These are closed public enums; their contiguous ranges are deliberately
    // checked before any serializer indexes a name table.
    if (static_cast<unsigned>(snapshot.retryability) >
            static_cast<unsigned>(Retryability::ManualRecoveryRequired) ||
        static_cast<unsigned>(snapshot.failure_domain) >
            static_cast<unsigned>(FailureDomain::Process) ||
        static_cast<unsigned>(snapshot.data_impact) >
            static_cast<unsigned>(DataImpact::HistoricalArtifactLostOrCorrupt)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return {};
}

Result<void> validate_history(const OperationHistory& history, const HistoryPolicy& policy) {
    if (policy.max_entries == 0 || policy.max_entries > 4096 ||
        policy.emergency_entries >= policy.max_entries || policy.recovery_window.count() < 0 ||
        history.entries.size() > policy.max_entries) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        std::set<OperationId> identities;
        std::set<std::string_view> keys;
        ControlGeneration previous = 0;
        for (const auto& operation : history.entries) {
            auto valid = validate_operation(operation);
            if (!valid.ok()) {
                return std::move(valid).status();
            }
            if (operation.snapshot.control_generation <= previous ||
                !identities.insert(operation.snapshot.operation_id).second ||
                !keys.insert(operation.request.request_key).second) {
                return Status(StatusCode::InvalidArgument, {});
            }
            previous = operation.snapshot.control_generation;
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationId> find_request(
    const OperationHistory& history, const RequestIdentity& request, std::string_view digest) {
    if (!validate_request_identity(request).ok() || !persistence::valid_digest(digest)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto found =
        std::find_if(history.entries.begin(), history.entries.end(), [&](const auto& operation) {
            return operation.request.request_key == request.request_key;
        });
    if (found == history.entries.end()) {
        return Status(StatusCode::NotFound, {});
    }
    if (found->request_digest != digest) {
        return Status(StatusCode::Conflict, {});
    }
    return found->snapshot.operation_id;
}

Result<OperationHistory> append_operation(
    const OperationHistory& history, StoredOperation operation,
    std::optional<OperationId> current_operation, const HistoryPolicy& policy, WallTime now) {
    auto valid = validate_history(history, policy);
    auto valid_new = validate_operation(operation);
    if (!valid.ok()) {
        return std::move(valid).status();
    }
    if (!valid_new.ok()) {
        return std::move(valid_new).status();
    }
    if (!history.entries.empty() && operation.snapshot.control_generation <=
                                        history.entries.back().snapshot.control_generation) {
        return Status(StatusCode::Conflict, {});
    }
    for (const auto& existing : history.entries) {
        if (existing.request.request_key == operation.request.request_key ||
            existing.snapshot.operation_id == operation.snapshot.operation_id) {
            return Status(StatusCode::Conflict, {});
        }
    }
    try {
        OperationHistory candidate = history;
        const auto budget = emergency(operation.snapshot.kind)
                                ? policy.max_entries
                                : policy.max_entries - policy.emergency_entries;
        if (candidate.entries.size() >= budget) {
            auto removed = std::remove_if(
                candidate.entries.begin(), candidate.entries.end(), [&](const auto& existing) {
                    return (!current_operation ||
                            !(existing.snapshot.operation_id == *current_operation)) &&
                           expired(existing.snapshot, now, policy.recovery_window);
                });
            candidate.history_truncated =
                candidate.history_truncated || removed != candidate.entries.end();
            candidate.entries.erase(removed, candidate.entries.end());
        }
        if (candidate.entries.size() >= budget) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        candidate.entries.push_back(std::move(operation));
        return candidate;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<OperationPage> operation_page(
    TaskId task_id, const OperationHistory& history, const PageRequest& request) {
    if (!valid_id(task_id) || request.limit == 0 || request.limit > 256 ||
        request.cursor.size() > 128) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        Cursor cursor;
        if (!request.cursor.empty()) {
            auto parsed = parse_cursor(task_id, request.cursor);
            if (!parsed.ok()) {
                return std::move(parsed).status();
            }
            cursor = parsed.value();
        }
        OperationPage page;
        page.history_truncated = history.history_truncated;
        if (!history.entries.empty()) {
            page.oldest_retained_control_generation =
                history.entries.front().snapshot.control_generation;
            page.oldest_retained_at = history.entries.front().snapshot.accepted_at;
        }
        for (const auto& stored : history.entries) {
            const auto& operation = stored.snapshot;
            if (operation.control_generation < cursor.generation ||
                (operation.control_generation == cursor.generation &&
                 !(cursor.operation_id < operation.operation_id))) {
                continue;
            }
            if (page.items.size() == request.limit) {
                page.has_more = true;
                const auto& last = page.items.back();
                page.next_cursor = "operations:v1:" + to_string(task_id) + ":" +
                                   std::to_string(last.control_generation) + ":" +
                                   to_string(last.operation_id);
                break;
            }
            page.items.push_back(operation);
        }
        return page;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::task
