#include "rodaq/runtime/resource_budget.hpp"

#include <mutex>
#include <utility>

namespace rodaq {

namespace {

bool exceeds_capacity(
    ResourceAmount used, ResourceAmount requested, ResourceAmount capacity) noexcept {
    const std::uint64_t available_records = capacity.records - used.records;
    const std::uint64_t available_bytes = capacity.bytes - used.bytes;
    return requested.records > available_records || requested.bytes > available_bytes;
}

}  // namespace

Reservation::Reservation(ResourceBudget& budget, ResourceAmount amount) noexcept
: budget_(&budget), amount_(amount) {}

Reservation::Reservation(Reservation&& other) noexcept
: budget_(other.budget_), amount_(other.amount_) {
    other.budget_ = nullptr;
    other.amount_ = {};
}

Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        Release();
        budget_ = other.budget_;
        amount_ = other.amount_;
        other.budget_ = nullptr;
        other.amount_ = {};
    }
    return *this;
}

Reservation::~Reservation() { Release(); }

void Reservation::Release() noexcept {
    if (budget_ != nullptr) {
        ResourceBudget* budget = budget_;
        const ResourceAmount amount = amount_;
        budget_ = nullptr;
        amount_ = {};
        budget->Release(amount);
    }
}

ResourceBudget::ResourceBudget(ResourceAmount capacity) noexcept : capacity_(capacity) {}

Result<Reservation> ResourceBudget::TryReserve(ResourceAmount requested) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ReserveResult result = TryReserveLocked(requested);
    if (!result.reserved) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "resource budget is exhausted"));
    }
    return Reservation(*this, requested);
}

ResourceAmount ResourceBudget::Used() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_;
}

ResourceAmount ResourceBudget::Available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ResourceAmount{
        capacity_.records - used_.records,
        capacity_.bytes - used_.bytes,
    };
}

ResourceAmount ResourceBudget::HighWater() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return high_water_;
}

std::uint64_t ResourceBudget::ActiveReservations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_reservations_;
}

ResourceBudget::ReserveResult ResourceBudget::TryReserveLocked(ResourceAmount requested) noexcept {
    if (exceeds_capacity(used_, requested, capacity_)) {
        return ReserveResult{false};
    }

    used_.records += requested.records;
    used_.bytes += requested.bytes;
    ++active_reservations_;
    if (used_.records > high_water_.records) {
        high_water_.records = used_.records;
    }
    if (used_.bytes > high_water_.bytes) {
        high_water_.bytes = used_.bytes;
    }
    return ReserveResult{true};
}

void ResourceBudget::Release(ResourceAmount amount) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    used_.records -= amount.records;
    used_.bytes -= amount.bytes;
    --active_reservations_;
    wait_condition_.notify_all();
}

void ResourceBudget::NotifyWaiters() noexcept { wait_condition_.notify_all(); }

}  // namespace rodaq
