#pragma once

#include <cstdint>
#include <optional>

#include <rodaq/result.hpp>

#include "stream_identity.hpp"

namespace rodaq::pipeline {

struct SequenceAllocationSnapshot {
    std::uint64_t active_remaining{0};
    std::uint64_t prepared_remaining{0};
    std::uint64_t refill_work_id{0};
    StatusCode refill_failure{StatusCode::Ok};
};

// One Recording Instance / Stream owner, synchronized by that Stream's short
// commit domain. No I/O, allocations, native callbacks, waits or threads.
// The work owner retains this instance until the actual refill call returns.
class SequenceAllocator {
public:
    SequenceAllocator() = default;
    SequenceAllocator(const SequenceAllocator&) = delete;
    SequenceAllocator& operator=(const SequenceAllocator&) = delete;
    SequenceAllocator(SequenceAllocator&&) = delete;
    SequenceAllocator& operator=(SequenceAllocator&&) = delete;

    // Submit only after this succeeds. A watchdog/observer timeout is not a
    // completion and must not call either finish method or release the work.
    Result<void> begin_refill(std::uint64_t work_id);
    // Called only for a matching StreamIdentityStore durable successful result.
    Result<void> finish_refill(std::uint64_t work_id, IdentityRange durable_range);
    Result<void> finish_failure(std::uint64_t work_id, StatusCode failure);

    // Admission Commit calls this only after every ordinary resource, COPY,
    // Gate/descriptor fence and queue capacity token has already succeeded.
    // A separate Descriptor-ID allocator may consume on proposal: a failed
    // prepare burns that interpretation ID, never an AcceptanceSequence.
    Result<std::uint64_t> allocate();
    SequenceAllocationSnapshot snapshot() const noexcept;

private:
    IdentityRange active_;
    std::optional<IdentityRange> prepared_;
    std::uint64_t reserved_frontier_{0};
    std::uint64_t last_work_id_{0};
    std::uint64_t refill_work_id_{0};
    StatusCode refill_failure_{StatusCode::Ok};
};

}  // namespace rodaq::pipeline
