#pragma once

#include <rodaq/plugin/c/abi.h>

#include <chrono>
#include <cstdint>
#include <limits>

namespace rodaq::plugin {

using DeadlineDuration = std::chrono::nanoseconds;

class Deadline final {
public:
    Deadline() noexcept : nanoseconds_(RODAQ_DEADLINE_INFINITE) {}

    [[nodiscard]] static Deadline infinite() noexcept { return Deadline{}; }

    [[nodiscard]] static Deadline after(DeadlineDuration duration) noexcept {
        if (duration.count() < 0) {
            return Deadline{0};
        }

        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto current = std::chrono::duration_cast<DeadlineDuration>(now).count();
        constexpr auto kMaxFinite = static_cast<std::int64_t>(RODAQ_DEADLINE_INFINITE - 1);
        if (duration.count() > kMaxFinite - current) {
            return Deadline{kMaxFinite};
        }
        return Deadline{current + duration.count()};
    }

    [[nodiscard]] static Deadline from_nanoseconds(std::int64_t value) noexcept {
        return Deadline{value};
    }

    [[nodiscard]] std::int64_t nanoseconds() const noexcept { return nanoseconds_; }

    [[nodiscard]] bool is_infinite() const noexcept {
        return nanoseconds_ == RODAQ_DEADLINE_INFINITE;
    }

    [[nodiscard]] bool expired() const noexcept {
        if (is_infinite()) {
            return false;
        }
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<DeadlineDuration>(now).count() >= nanoseconds_;
    }

private:
    explicit Deadline(std::int64_t nanoseconds) noexcept : nanoseconds_(nanoseconds) {}

    std::int64_t nanoseconds_;
};

[[nodiscard]] inline rodaq_deadline_t to_abi_deadline(Deadline deadline) noexcept {
    return rodaq_deadline_t{deadline.nanoseconds()};
}

}  // namespace rodaq::plugin
