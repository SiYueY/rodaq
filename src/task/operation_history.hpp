#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rodaq/task/config.hpp>
#include <rodaq/task/types.hpp>

namespace rodaq::task {

struct HistoryPolicy {
    std::size_t max_entries{64};
    std::chrono::nanoseconds ttl{std::chrono::hours{1}};
};

// Typed request signature for in-process duplicate suppression (no JCS/digest).
struct RequestSignature {
    OperationKind kind{OperationKind::Stop};
    ControlPrecondition precondition;
    std::optional<TaskConfig> config;

    bool matches(const RequestSignature& other) const noexcept;
};

struct RegisteredOperation {
    std::uint64_t acceptance_order{0};
    std::string request_key;
    RequestSignature signature;
    OperationSnapshot snapshot;
};

bool is_terminal(OperationState state) noexcept;

// Bounded process-local Operation Registry for one Task.
class OperationRegistry {
public:
    explicit OperationRegistry(HistoryPolicy policy = {}) noexcept;

    // Lookup by request_key before checking current control_generation.
    // Returns NotFound if absent, Conflict if key exists with different
    // RequestSignature, or the existing OperationId if exact match.
    Result<OperationId> find_request(
        std::string_view request_key, const RequestSignature& signature, WallTime now);

    // Records a newly accepted Operation, evicting oldest terminal entries
    // exceeding max_entries or TTL.
    Result<void> record(
        std::string request_key, RequestSignature signature, OperationSnapshot snapshot,
        WallTime now);

    bool update(const OperationSnapshot& updated) noexcept;
    Result<OperationSnapshot> get(OperationId id, WallTime now) const;
    Result<OperationPage> list(TaskId task_id, const PageRequest& request, WallTime now) const;

private:
    void prune(WallTime now) noexcept;

    HistoryPolicy policy_;
    std::uint64_t next_order_{1};
    std::deque<RegisteredOperation> entries_;
    bool truncated_{false};
};

// Bounded process-local Create idempotency cache (cleared on restart).
class CreateRegistry {
public:
    explicit CreateRegistry(HistoryPolicy policy = {}) noexcept;

    // Returns NotFound if absent, Conflict if key exists with different config,
    // or the original TaskId if identical config.
    Result<TaskId> find(std::string_view request_key, const TaskConfig& config, WallTime now);
    Result<void> record(std::string request_key, TaskConfig config, TaskId task_id, WallTime now);

private:
    struct Entry {
        std::string request_key;
        TaskConfig config;
        TaskId task_id;
        WallTime accepted_at;
    };
    void prune(WallTime now) noexcept;

    HistoryPolicy policy_;
    std::deque<Entry> entries_;
};

}  // namespace rodaq::task
