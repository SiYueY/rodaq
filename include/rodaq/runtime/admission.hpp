#ifndef RODAQ_RUNTIME_ADMISSION_HPP_
#define RODAQ_RUNTIME_ADMISSION_HPP_

#include <rodaq/runtime/resource_budget.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rodaq {

enum class AdmissionMode {
    Reject,
    Wait,
};

enum class AdmissionGateOutcome {
    Granted,
    ResourceUnavailable,
    Timeout,
    Cancelled,
    GateClosed,
};

struct AdmissionRequest {
    ResourceAmount resources{};
    AdmissionMode mode = AdmissionMode::Reject;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct AdmissionGateDecision {
    AdmissionGateOutcome outcome = AdmissionGateOutcome::ResourceUnavailable;
    Reservation reservation;
};

/// Cancels a waiting AdmissionGate operation and wakes its waiters.
///
/// The ResourceBudget and AdmissionGate must outlive the token and all
/// operations using it.
class AdmissionCancellation final {
public:
    explicit AdmissionCancellation(ResourceBudget& budget) noexcept;

    AdmissionCancellation(const AdmissionCancellation&) = delete;
    AdmissionCancellation& operator=(const AdmissionCancellation&) = delete;
    AdmissionCancellation(AdmissionCancellation&&) = delete;
    AdmissionCancellation& operator=(AdmissionCancellation&&) = delete;

    void Cancel() noexcept;
    bool IsCancelled() const noexcept;

private:
    friend class AdmissionGate;

    ResourceBudget* budget_;
    std::atomic<bool> cancelled_{false};
};

/// Serializes resource admission with Gate open/close transitions.
///
/// The ResourceBudget must outlive the gate. Admission decisions return a
/// normal domain outcome; Result failure is reserved for invalid API usage.
class AdmissionGate final {
public:
    explicit AdmissionGate(ResourceBudget& budget) noexcept;
    AdmissionGate(ResourceBudget& budget, std::uint64_t initial_generation) noexcept;

    AdmissionGate(const AdmissionGate&) = delete;
    AdmissionGate& operator=(const AdmissionGate&) = delete;
    AdmissionGate(AdmissionGate&&) = delete;
    AdmissionGate& operator=(AdmissionGate&&) = delete;

    Result<AdmissionGateDecision> Admit(const AdmissionRequest& request);
    Result<AdmissionGateDecision> Admit(
        const AdmissionRequest& request, const AdmissionCancellation& cancellation);

    void Open() noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept;
    std::uint64_t Generation() const noexcept;
    std::size_t WaitingCount() const noexcept;

private:
    Result<AdmissionGateDecision> AdmitWithCancellation(
        const AdmissionRequest& request, const AdmissionCancellation* cancellation);
    bool AdvanceGenerationLocked() noexcept;

    ResourceBudget& budget_;
    bool open_ = false;
    std::uint64_t generation_ = 0;
    std::size_t waiting_count_ = 0;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_ADMISSION_HPP_
