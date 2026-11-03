#include "rodaq/runtime/admission.hpp"

#include <chrono>
#include <limits>
#include <mutex>

namespace rodaq {

namespace {

AdmissionGateDecision decision(AdmissionGateOutcome outcome) {
    return AdmissionGateDecision{outcome, Reservation()};
}

}  // namespace

AdmissionCancellation::AdmissionCancellation(ResourceBudget& budget) noexcept : budget_(&budget) {}

void AdmissionCancellation::Cancel() noexcept {
    cancelled_.store(true);
    budget_->NotifyWaiters();
}

bool AdmissionCancellation::IsCancelled() const noexcept { return cancelled_.load(); }

AdmissionGate::AdmissionGate(ResourceBudget& budget) noexcept : budget_(budget) {}

AdmissionGate::AdmissionGate(ResourceBudget& budget, std::uint64_t initial_generation) noexcept
: budget_(budget), generation_(initial_generation) {}

Result<AdmissionGateDecision> AdmissionGate::Admit(const AdmissionRequest& request) {
    return AdmitWithCancellation(request, nullptr);
}

Result<AdmissionGateDecision> AdmissionGate::Admit(
    const AdmissionRequest& request, const AdmissionCancellation& cancellation) {
    return AdmitWithCancellation(request, &cancellation);
}

void AdmissionGate::Open() noexcept {
    std::lock_guard<std::mutex> lock(budget_.mutex_);
    if (open_ || generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return;
    }
    open_ = AdvanceGenerationLocked();
    budget_.wait_condition_.notify_all();
}

void AdmissionGate::Close() noexcept {
    std::lock_guard<std::mutex> lock(budget_.mutex_);
    if (open_) {
        open_ = false;
        static_cast<void>(AdvanceGenerationLocked());
        budget_.wait_condition_.notify_all();
    }
}

bool AdmissionGate::IsOpen() const noexcept {
    std::lock_guard<std::mutex> lock(budget_.mutex_);
    return open_;
}

std::uint64_t AdmissionGate::Generation() const noexcept {
    std::lock_guard<std::mutex> lock(budget_.mutex_);
    return generation_;
}

std::size_t AdmissionGate::WaitingCount() const noexcept {
    std::lock_guard<std::mutex> lock(budget_.mutex_);
    return waiting_count_;
}

Result<AdmissionGateDecision> AdmissionGate::AdmitWithCancellation(
    const AdmissionRequest& request, const AdmissionCancellation* cancellation) {
    if (request.mode != AdmissionMode::Reject && request.mode != AdmissionMode::Wait) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "admission mode is invalid"));
    }
    if (request.mode == AdmissionMode::Wait && !request.deadline.has_value()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "waiting admission requires a deadline"));
    }
    if (cancellation != nullptr && cancellation->budget_ != &budget_) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument,
            "admission cancellation belongs to a different resource budget"));
    }

    std::unique_lock<std::mutex> lock(budget_.mutex_);
    if (!open_) {
        return decision(AdmissionGateOutcome::GateClosed);
    }
    if (cancellation != nullptr && cancellation->IsCancelled()) {
        return decision(AdmissionGateOutcome::Cancelled);
    }

    if (request.mode == AdmissionMode::Reject) {
        const ResourceBudget::ReserveResult reserved = budget_.TryReserveLocked(request.resources);
        if (!reserved.reserved) {
            return decision(AdmissionGateOutcome::ResourceUnavailable);
        }
        return AdmissionGateDecision{
            AdmissionGateOutcome::Granted, Reservation(budget_, request.resources)};
    }

    const std::uint64_t waiting_generation = generation_;
    const std::chrono::steady_clock::time_point deadline = *request.deadline;
    while (true) {
        if (cancellation != nullptr && cancellation->IsCancelled()) {
            return decision(AdmissionGateOutcome::Cancelled);
        }
        if (!open_ || generation_ != waiting_generation) {
            return decision(AdmissionGateOutcome::GateClosed);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return decision(AdmissionGateOutcome::Timeout);
        }

        const ResourceBudget::ReserveResult reserved = budget_.TryReserveLocked(request.resources);
        if (reserved.reserved) {
            return AdmissionGateDecision{
                AdmissionGateOutcome::Granted, Reservation(budget_, request.resources)};
        }
        ++waiting_count_;
        budget_.wait_condition_.wait_until(lock, deadline);
        --waiting_count_;
    }
}

bool AdmissionGate::AdvanceGenerationLocked() noexcept {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max() - 1U) {
        generation_ = std::numeric_limits<std::uint64_t>::max();
        return false;
    }
    ++generation_;
    return true;
}

}  // namespace rodaq
