#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <rodaq/task/types.hpp>

#include "../persistence/atomic_file.hpp"

namespace rodaq::task {

enum class CreatePhase { Allocated, Materializing, Completed };

struct CreateReceipt {
    std::string request_key_hash;
    std::string request_digest;
    TaskId assigned_task_id;
    CreatePhase phase{CreatePhase::Allocated};
    WallTime created_at;
};

enum class CreateClaimState { Claimed, ExistingInProgress, ExistingCompleted };

struct CreateClaim {
    CreateClaimState state{CreateClaimState::Claimed};
    CreateReceipt receipt;
};

// This owner is used on the bounded Create persistence lane, never the Task
// control strand. Its prepared directory is exclusively leased for its lifetime.
// Concurrent calls fail immediately; there is no hidden queue or native thread.
class TaskCreateStore {
public:
    static Result<std::unique_ptr<TaskCreateStore>> open(
        const std::filesystem::path& directory, std::size_t max_receipts,
        persistence::FileFaultHook faults = {});

    TaskCreateStore(const TaskCreateStore&) = delete;
    TaskCreateStore& operator=(const TaskCreateStore&) = delete;
    ~TaskCreateStore() noexcept;

    Result<CreateClaim> claim(
        std::string_view request_key_hash, std::string_view request_digest,
        TaskId candidate_task_id, WallTime created_at);
    Result<void> publish(const CreateReceipt& receipt);

    // Required after any uncertain write. Validate and sync actual authority
    // before admitting more mutations; never infer durability from read alone.
    Result<void> reconcile();

private:
    struct State;
    explicit TaskCreateStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::task
