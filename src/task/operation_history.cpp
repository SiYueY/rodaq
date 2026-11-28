#include "operation_history.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "../persistence/decimal.hpp"
#include "../uuid.hpp"
#include "config.hpp"

namespace rodaq::task {

bool is_terminal(OperationState state) noexcept {
    return state == OperationState::Succeeded || state == OperationState::PartiallySucceeded ||
           state == OperationState::Failed || state == OperationState::Superseded;
}

bool RequestSignature::matches(const RequestSignature& other) const noexcept {
    if (kind != other.kind ||
        precondition.has_expected_control_generation !=
            other.precondition.has_expected_control_generation ||
        (precondition.has_expected_control_generation &&
         precondition.expected_control_generation !=
             other.precondition.expected_control_generation) ||
        config.has_value() != other.config.has_value()) {
        return false;
    }
    return !config || equal_task_config(*config, *other.config);
}

OperationRegistry::OperationRegistry(HistoryPolicy policy) noexcept : policy_(policy) {
    if (policy_.max_entries == 0) {
        policy_.max_entries = 64;
    }
}

void OperationRegistry::prune(WallTime now) noexcept {
    if (policy_.ttl > std::chrono::nanoseconds::zero()) {
        while (!entries_.empty()) {
            const auto& front = entries_.front();
            if (!is_terminal(front.snapshot.state)) {
                break;
            }
            const auto ref_time = front.snapshot.terminal_at.value_or(front.snapshot.accepted_at);
            if (now <= ref_time || (now - ref_time) < policy_.ttl) {
                break;
            }
            entries_.pop_front();
            truncated_ = true;
        }
    }
    while (entries_.size() > policy_.max_entries) {
        const auto it = std::find_if(entries_.begin(), entries_.end(), [](const auto& item) {
            return is_terminal(item.snapshot.state);
        });
        if (it == entries_.end()) {
            break;
        }
        entries_.erase(it);
        truncated_ = true;
    }
}

Result<OperationId> OperationRegistry::find_request(
    std::string_view request_key, const RequestSignature& signature, WallTime now) {
    prune(now);
    for (const auto& entry : entries_) {
        if (entry.request_key == request_key) {
            if (!entry.signature.matches(signature)) {
                return Status(
                    StatusCode::Conflict, "request_key already used for a different request");
            }
            return entry.snapshot.operation_id;
        }
    }
    return Status(StatusCode::NotFound, {});
}

Result<void> OperationRegistry::record(
    std::string request_key, RequestSignature signature, OperationSnapshot snapshot, WallTime now) {
    try {
        prune(now);
        if (entries_.size() >= policy_.max_entries) {
            const auto it = std::find_if(entries_.begin(), entries_.end(), [](const auto& item) {
                return is_terminal(item.snapshot.state);
            });
            if (it == entries_.end()) {
                return Status(StatusCode::ResourceExhausted, "Operation registry full");
            }
            entries_.erase(it);
            truncated_ = true;
        }
        entries_.push_back(RegisteredOperation{
            next_order_++, std::move(request_key), std::move(signature), std::move(snapshot)});
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

bool OperationRegistry::update(const OperationSnapshot& updated) noexcept {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->snapshot.operation_id == updated.operation_id) {
            it->snapshot = updated;
            return true;
        }
    }
    return false;
}

Result<OperationSnapshot> OperationRegistry::get(OperationId id, WallTime now) const {
    if (!uuid::nonzero(id.value)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->snapshot.operation_id == id) {
            if (is_terminal(it->snapshot.state) && policy_.ttl > std::chrono::nanoseconds::zero()) {
                const auto ref_time = it->snapshot.terminal_at.value_or(it->snapshot.accepted_at);
                if (now > ref_time && (now - ref_time) >= policy_.ttl) {
                    return Status(StatusCode::NotFound, {});
                }
            }
            return it->snapshot;
        }
    }
    return Status(StatusCode::NotFound, {});
}

Result<OperationPage> OperationRegistry::list(
    TaskId task_id, const PageRequest& request, WallTime now) const {
    if (!uuid::nonzero(task_id.value) || request.limit == 0 || request.limit > 256 ||
        request.cursor.size() > 256) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::optional<std::pair<std::uint64_t, OperationId>> cursor_key;
    if (!request.cursor.empty()) {
        const std::string prefix = "op1:" + to_string(task_id) + ":";
        if (request.cursor.size() <= prefix.size() ||
            std::string_view(request.cursor).substr(0, prefix.size()) != prefix) {
            return Status(StatusCode::InvalidArgument, {});
        }
        std::string_view rest = std::string_view(request.cursor).substr(prefix.size());
        const auto sep = rest.find(':');
        if (sep == std::string_view::npos) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto order = persistence::unsigned_decimal(rest.substr(0, sep));
        auto op_id = parse_operation_id(rest.substr(sep + 1));
        if (!order.ok() || order.value() == 0 || !op_id.ok()) {
            return Status(StatusCode::InvalidArgument, {});
        }
        cursor_key = std::pair{order.value(), op_id.value()};
    }

    try {
        // Return in descending acceptance order (newest first) with operation_id tie-break.
        std::vector<const RegisteredOperation*> matched;
        matched.reserve(entries_.size());
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (is_terminal(it->snapshot.state) && policy_.ttl > std::chrono::nanoseconds::zero()) {
                const auto ref_time = it->snapshot.terminal_at.value_or(it->snapshot.accepted_at);
                if (now > ref_time && (now - ref_time) >= policy_.ttl) {
                    continue;
                }
            }
            if (cursor_key) {
                const bool after = it->acceptance_order < cursor_key->first ||
                                   (it->acceptance_order == cursor_key->first &&
                                    it->snapshot.operation_id < cursor_key->second);
                if (!after) {
                    continue;
                }
            }
            matched.push_back(&(*it));
        }

        OperationPage page;
        page.history_truncated = truncated_;
        const std::size_t count = std::min<std::size_t>(matched.size(), request.limit);
        page.items.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            page.items.push_back(matched[i]->snapshot);
        }
        page.has_more = matched.size() > count;
        if (page.has_more && count > 0) {
            const auto* last = matched[count - 1];
            page.next_cursor = "op1:" + to_string(task_id) + ":" +
                               std::to_string(last->acceptance_order) + ":" +
                               to_string(last->snapshot.operation_id);
        }
        return page;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

CreateRegistry::CreateRegistry(HistoryPolicy policy) noexcept : policy_(policy) {
    if (policy_.max_entries == 0) {
        policy_.max_entries = 64;
    }
}

void CreateRegistry::prune(WallTime now) noexcept {
    if (policy_.ttl > std::chrono::nanoseconds::zero()) {
        while (!entries_.empty()) {
            const auto& front = entries_.front();
            if (now <= front.accepted_at || (now - front.accepted_at) < policy_.ttl) {
                break;
            }
            entries_.pop_front();
        }
    }
    while (entries_.size() > policy_.max_entries) {
        entries_.pop_front();
    }
}

Result<TaskId> CreateRegistry::find(
    std::string_view request_key, const TaskConfig& config, WallTime now) {
    prune(now);
    for (const auto& entry : entries_) {
        if (entry.request_key == request_key) {
            if (!equal_task_config(entry.config, config)) {
                return Status(
                    StatusCode::Conflict,
                    "request_key already used for a different create_task config");
            }
            return entry.task_id;
        }
    }
    return Status(StatusCode::NotFound, {});
}

Result<void> CreateRegistry::record(
    std::string request_key, TaskConfig config, TaskId task_id, WallTime now) {
    try {
        prune(now);
        if (entries_.size() >= policy_.max_entries) {
            entries_.pop_front();
        }
        entries_.push_back(Entry{std::move(request_key), std::move(config), task_id, now});
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::task
