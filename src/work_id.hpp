#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

#include <rodaq/result.hpp>

namespace rodaq {

// One process-wide Core work identity space. Zero is invalid; exhaustion never
// wraps into a previously used identity. Persisted domain identities are separate.
inline Result<std::uint64_t> next_work_id() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
    return Status(StatusCode::ResourceExhausted, {});
}

}  // namespace rodaq
