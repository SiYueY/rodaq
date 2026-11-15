#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>

#include "stream_identity.hpp"
#include "../persistence/atomic_file.hpp"

namespace rodaq::pipeline {

enum class IdentityStoreMode { Materializing, Existing };

// Task-scoped identity authority, independent of Recording Instance and Reset.
// Composition uses bounded persistence execution. Concurrent mutations are
// rejected before I/O; the owner schedules retries without blocking Admission.
class StreamIdentityStore {
public:
    static Result<std::unique_ptr<StreamIdentityStore>> open(
        const std::filesystem::path& directory, TaskId task_id, IdentityStoreMode mode,
        std::size_t max_identities, persistence::FileFaultHook faults = {});

    StreamIdentityStore(const StreamIdentityStore&) = delete;
    StreamIdentityStore& operator=(const StreamIdentityStore&) = delete;
    ~StreamIdentityStore() noexcept;

    Result<void> initialize(const StreamIdentityProjection& projection);
    // Task Control remains the sole authority selecting an active Config.
    // On boot it may select the previous Config after an unaccepted Configure;
    // watermarks and all previously reserved sequence ranges never roll back.
    Result<void> apply_config_change(
        const StreamIdentityProjection& projection, std::uint64_t expected_revision);
    Result<IdentityRange> reserve(StreamId stream_id, std::uint32_t requested_count);
    Result<IdentityRange> reserve_descriptors(StreamId stream_id, std::uint32_t requested_count);
    Result<StreamIdentityState> load();
    Result<std::optional<StreamIdentityState>> reconcile();

private:
    Result<IdentityRange> reserve_ids(
        StreamId stream_id, std::uint32_t requested_count, bool descriptor_ids);
    struct State;
    explicit StreamIdentityStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

}  // namespace rodaq::pipeline
