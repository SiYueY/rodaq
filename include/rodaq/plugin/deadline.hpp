#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <time.h>

#include <rodaq/c/types.h>
#include <rodaq/result.hpp>

namespace rodaq::plugin {

using DeadlineDuration = std::chrono::nanoseconds;

/** Absolute Linux CLOCK_MONOTONIC deadline; expiration never cancels native work. */
class Deadline {
public:
    [[nodiscard]] static constexpr Deadline infinite() noexcept {
        return Deadline(RODAQ_DEADLINE_INFINITE);
    }

    [[nodiscard]] static Result<Deadline> from_native(rodaq_deadline_t value) {
        if (value.monotonic_deadline < 0) {
            return Status(StatusCode::InvalidArgument, "Negative monotonic deadline");
        }
        return Deadline(value.monotonic_deadline);
    }

    /** Finite waits saturate to INT64_MAX-1, never to the infinite sentinel. */
    [[nodiscard]] static Result<Deadline> after(std::chrono::nanoseconds duration) {
        if (duration.count() < 0) {
            return Status(StatusCode::InvalidArgument, "Negative wait duration");
        }
        const auto now = monotonic_now();
        if (!now.ok()) {
            return now.status();
        }
        constexpr auto kLatest = std::numeric_limits<std::int64_t>::max() - 1;
        const auto room = kLatest - now.value();
        return Deadline(duration.count() >= room ? kLatest : now.value() + duration.count());
    }

    [[nodiscard]] constexpr rodaq_deadline_t native() const noexcept { return {value_}; }

    [[nodiscard]] constexpr bool is_infinite() const noexcept {
        return value_ == RODAQ_DEADLINE_INFINITE;
    }

    [[nodiscard]] Result<std::chrono::nanoseconds> remaining() const {
        if (is_infinite()) {
            return std::chrono::nanoseconds::max();
        }
        const auto now = monotonic_now();
        if (!now.ok()) {
            return now.status();
        }
        return std::chrono::nanoseconds(value_ > now.value() ? value_ - now.value() : 0);
    }

private:
    explicit constexpr Deadline(std::int64_t value) noexcept : value_(value) {}

    static Result<std::int64_t> monotonic_now() {
        timespec now{};
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
            now.tv_nsec >= 1000000000) {
            return Status(StatusCode::Internal, "CLOCK_MONOTONIC unavailable");
        }
        constexpr std::int64_t kLatest = std::numeric_limits<std::int64_t>::max() - 1;
        if (now.tv_sec > kLatest / 1000000000 ||
            (now.tv_sec == kLatest / 1000000000 && now.tv_nsec > kLatest % 1000000000)) {
            return kLatest;
        }
        return static_cast<std::int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
    }

    std::int64_t value_;
};

}  // namespace rodaq::plugin
