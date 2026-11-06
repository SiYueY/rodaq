#ifndef RODAQ_ADMISSION_ADMISSION_HPP_
#define RODAQ_ADMISSION_ADMISSION_HPP_

#include "admission/resource_budget.hpp"
#include <rodaq/daq_config.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rodaq {

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

    void cancel() noexcept;
    bool is_cancelled() const noexcept;

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

    Result<AdmissionGateDecision> admit(const AdmissionRequest& request);
    Result<AdmissionGateDecision> admit(
        const AdmissionRequest& request, const AdmissionCancellation& cancellation);

    void open() noexcept;
    void close() noexcept;
    bool is_open() const noexcept;
    std::uint64_t generation() const noexcept;
    std::size_t waiting_count() const noexcept;

private:
    Result<AdmissionGateDecision> admit_with_cancellation(
        const AdmissionRequest& request, const AdmissionCancellation* cancellation);
    bool advance_generation_locked() noexcept;

    ResourceBudget& budget_;
    bool open_ = false;
    std::uint64_t generation_ = 0;
    std::size_t waiting_count_ = 0;
};

}  // namespace rodaq

#endif  // RODAQ_ADMISSION_ADMISSION_HPP_
