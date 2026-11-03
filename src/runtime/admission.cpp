#include "rodaq/runtime/admission.hpp"

#include <chrono>
#include <mutex>

namespace rodaq {

namespace {

AdmissionDecision decision(AdmissionOutcome outcome, AdmissionRejectionReason reason) {
    return AdmissionDecision{outcome, reason, Reservation()};
}

}  // namespace

AdmissionCancellation::AdmissionCancellation(ResourceBudget& budget) noexcept : budget_(&budget) {}

void AdmissionCancellation::Cancel() noexcept {
    cancelled_.store(true);
    budget_->NotifyWaiters();
}

bool AdmissionCancellation::IsCancelled() const noexcept { return cancelled_.load(); }

AdmissionGate::AdmissionGate(ResourceBudget& budget) noexcept : budget_(budget) {}

Result<AdmissionDecision> AdmissionGate::Admit(const AdmissionRequest& request) {
    return AdmitWithCancellation(request, nullptr);
}

Result<AdmissionDecision> AdmissionGate::Admit(
    const AdmissionRequest& request, const AdmissionCancellation& cancellation) {
    return AdmitWithCancellation(request, &cancellation);
}

void AdmissionGate::Open() noexcept {
    std::lock_guard<std::mutex> lock(budget_.mutex_);
    if (!open_) {
        open_ = true;
        ++generation_;
        budget_.wait_condition_.notify_all();
    }
}

void AdmissionGate::Close() noexcept {
    std::lock_guard<std::mutex> lock(budget_.mutex_);
    if (open_) {
        open_ = false;
        ++generation_;
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

Result<AdmissionDecision> AdmissionGate::AdmitWithCancellation(
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
        return decision(AdmissionOutcome::GateClosed, AdmissionRejectionReason::GateClosed);
    }
    if (cancellation != nullptr && cancellation->IsCancelled()) {
        return decision(AdmissionOutcome::Cancelled, AdmissionRejectionReason::Cancelled);
    }

    if (request.mode == AdmissionMode::Reject) {
        const ResourceBudget::ReserveResult reserved = budget_.TryReserveLocked(request.resources);
        if (!reserved.reserved) {
            return decision(
                AdmissionOutcome::Rejected, AdmissionRejectionReason::ResourceUnavailable);
        }
        return AdmissionDecision{
            AdmissionOutcome::Accepted,
            AdmissionRejectionReason::None,
            Reservation(budget_, request.resources),
        };
    }

    const std::uint64_t waiting_generation = generation_;
    const std::chrono::steady_clock::time_point deadline = *request.deadline;
    while (true) {
        if (cancellation != nullptr && cancellation->IsCancelled()) {
            return decision(AdmissionOutcome::Cancelled, AdmissionRejectionReason::Cancelled);
        }
        if (!open_ || generation_ != waiting_generation) {
            return decision(AdmissionOutcome::GateClosed, AdmissionRejectionReason::GateClosed);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return decision(AdmissionOutcome::Timeout, AdmissionRejectionReason::WaitTimeout);
        }

        const ResourceBudget::ReserveResult reserved = budget_.TryReserveLocked(request.resources);
        if (reserved.reserved) {
            return AdmissionDecision{
                AdmissionOutcome::Accepted,
                AdmissionRejectionReason::None,
                Reservation(budget_, request.resources),
            };
        }
        ++waiting_count_;
        budget_.wait_condition_.wait_until(lock, deadline);
        --waiting_count_;
    }
}

}  // namespace rodaq
