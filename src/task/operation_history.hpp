#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/task/types.hpp>

namespace rodaq::task {

struct StoredOperation {
    RequestIdentity request;
    std::string request_digest;
    OperationSnapshot snapshot;
};

struct OperationHistory {
    std::vector<StoredOperation> entries;
    bool history_truncated{false};
};

struct HistoryPolicy {
    std::size_t max_entries{64};
    std::size_t emergency_entries{4};
    std::chrono::nanoseconds recovery_window{std::chrono::hours{24}};
};

bool terminal(OperationState state) noexcept;
Result<void> validate_operation(const StoredOperation& operation);
Result<void> validate_history(const OperationHistory& history, const HistoryPolicy& policy);

// The controller calls this before checking the current generation. An existing
// accepted request must remain retryable after it advanced its own generation.
Result<OperationId> find_request(
    const OperationHistory& history, const RequestIdentity& request, std::string_view digest);

// Produces a candidate value only. The caller publishes it through Task Control
// authority before replacing the current snapshot or returning Accepted.
Result<OperationHistory> append_operation(
    const OperationHistory& history, StoredOperation operation,
    std::optional<OperationId> current_operation, const HistoryPolicy& policy, WallTime now);

Result<OperationPage> operation_page(
    TaskId task_id, const OperationHistory& history, const PageRequest& request);

}  // namespace rodaq::task
