#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include <rodaq/task/config.hpp>

#include "instance_id.hpp"

namespace rodaq::pipeline {

enum class GateOpenResult { Opened, Obsolete };

struct GateSnapshot {
    InstanceId instance_id;
    ControlGeneration authority_generation{0};
    bool open{false};
    bool control_fence{true};
    std::uint32_t commit_inflight{0};
    std::uint64_t accepted_records{0};
};

class AdmissionGate;

// Non-allocating short Core critical section only. The owning instance must
// outlive this guard. Plugin calls, COPY, blocking I/O and unknown callbacks
// are forbidden while it is held; Stream commit locks are acquired after it.
class AdmissionCommit {
public:
    AdmissionCommit(const AdmissionCommit&) = delete;
    AdmissionCommit& operator=(const AdmissionCommit&) = delete;
    AdmissionCommit(AdmissionCommit&& other) noexcept;
    AdmissionCommit& operator=(AdmissionCommit&& other) noexcept;
    ~AdmissionCommit() noexcept;

    // Call after the capacity-token-backed non-failing queue publication.
    // Returns false on an internal duplicate mark; it never counts twice.
    bool mark_committed() noexcept;

private:
    friend class AdmissionGate;
    AdmissionCommit(AdmissionGate& gate, std::unique_lock<std::mutex> lock) noexcept;
    void release() noexcept;

    AdmissionGate* gate_{nullptr};
    std::unique_lock<std::mutex> lock_;
    bool committed_{false};
};

// A Gate belongs to exactly one actual Recording Instance. A control fence is
// permanent for that instance: explicit re-admission uses a new instance/Gate.
// No old Open or late completion can clear it as a side effect of persistence.
class AdmissionGate {
public:
    AdmissionGate(InstanceId instance_id, ControlGeneration authority_generation) noexcept;
    AdmissionGate(const AdmissionGate&) = delete;
    AdmissionGate& operator=(const AdmissionGate&) = delete;

    GateOpenResult open_admission(InstanceId instance_id, ControlGeneration generation);
    Result<AdmissionCommit> enter_commit(InstanceId instance_id, ControlGeneration generation);
    // Rotation takes this same short domain, closes the old Segment and swaps
    // its writable cursor. No old commit can publish after the cut. Release
    // before Seal, queue drain, Native calls or I/O; does not fence the instance.
    Result<std::unique_lock<std::mutex>> enter_segment_cut(
        InstanceId instance_id, ControlGeneration generation);

    // This atomic fence precedes the durable Pause/Stop/cleanup intent. It does
    // not wait for a Native call, filesystem operation, or current Core commit.
    Result<void> establish_control_fence(InstanceId instance_id) noexcept;
    Result<void> advance_authority(InstanceId instance_id, ControlGeneration generation);
    // Captures the frozen accepted set after existing short Core guards exit.
    Result<GateSnapshot> close_and_capture(InstanceId instance_id);
    GateSnapshot snapshot() const;
    InstanceId instance_id() const noexcept { return instance_id_; }

    bool admission_fenced() const noexcept {
        return !valid_ || control_fence_.load(std::memory_order_acquire);
    }

private:
    friend class AdmissionCommit;
    GateSnapshot snapshot_locked() const noexcept;

    const InstanceId instance_id_;
    const bool valid_;
    mutable std::mutex mutex_;
    ControlGeneration authority_generation_;
    bool open_{false};
    std::atomic<bool> control_fence_{false};
    std::uint32_t commit_inflight_{0};
    std::uint64_t accepted_records_{0};
};

}  // namespace rodaq::pipeline
