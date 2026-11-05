#ifndef RODAQ_RUNTIME_RESOURCE_BUDGET_HPP_
#define RODAQ_RUNTIME_RESOURCE_BUDGET_HPP_

#include <rodaq/result.hpp>

#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace rodaq {

struct ResourceAmount {
    std::uint64_t records = 0;
    std::uint64_t bytes = 0;
};

constexpr bool operator==(ResourceAmount lhs, ResourceAmount rhs) noexcept {
    return lhs.records == rhs.records && lhs.bytes == rhs.bytes;
}

class AdmissionCancellation;
class AdmissionGate;
class ResourceBudget;

/// Move-only ownership of a successful ResourceBudget reservation.
///
/// The originating ResourceBudget must outlive every Reservation created from
/// it. Destruction, explicit Release, and move assignment release at most once.
class Reservation final {
public:
    Reservation() noexcept = default;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    ~Reservation();

    bool active() const noexcept { return budget_ != nullptr; }

    ResourceAmount amount() const noexcept { return amount_; }

    void release() noexcept;

private:
    friend class AdmissionGate;
    friend class ResourceBudget;

    Reservation(ResourceBudget& budget, ResourceAmount amount) noexcept;

    ResourceBudget* budget_ = nullptr;
    ResourceAmount amount_{};
};

/// Thread-safe all-or-nothing accounting for bounded Runtime resources.
class ResourceBudget final {
public:
    explicit ResourceBudget(ResourceAmount capacity) noexcept;

    ResourceBudget(const ResourceBudget&) = delete;
    ResourceBudget& operator=(const ResourceBudget&) = delete;
    ResourceBudget(ResourceBudget&&) = delete;
    ResourceBudget& operator=(ResourceBudget&&) = delete;

    Result<Reservation> try_reserve(ResourceAmount requested);

    ResourceAmount capacity() const noexcept { return capacity_; }
    ResourceAmount used() const;
    ResourceAmount available() const;
    ResourceAmount high_water() const;
    std::uint64_t active_reservations() const;

private:
    friend class AdmissionCancellation;
    friend class AdmissionGate;
    friend class Reservation;

    struct ReserveResult {
        bool reserved = false;
    };

    ReserveResult try_reserve_locked(ResourceAmount requested) noexcept;
    void release(ResourceAmount amount) noexcept;
    void notify_waiters() noexcept;

    ResourceAmount capacity_{};
    mutable std::mutex mutex_;
    std::condition_variable wait_condition_;
    ResourceAmount used_{};
    ResourceAmount high_water_{};
    std::uint64_t active_reservations_ = 0;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_RESOURCE_BUDGET_HPP_
