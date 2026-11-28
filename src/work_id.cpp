#include "work_id.hpp"

#include <atomic>
#include <limits>

namespace rodaq {

Result<std::uint64_t> next_work_id() {
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
