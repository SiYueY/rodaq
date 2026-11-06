#ifndef RODAQ_PAYLOAD_POOL_STATE_HPP_
#define RODAQ_PAYLOAD_POOL_STATE_HPP_

#include "payload/payload_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rodaq {

class PayloadPoolState final : public std::enable_shared_from_this<PayloadPoolState> {
public:
    explicit PayloadPoolState(const PayloadPoolConfig& config);

    PayloadPoolState(const PayloadPoolState&) = delete;
    PayloadPoolState& operator=(const PayloadPoolState&) = delete;
    PayloadPoolState(PayloadPoolState&&) = delete;
    PayloadPoolState& operator=(PayloadPoolState&&) = delete;

    Result<PayloadPoolReservation> try_reserve(std::size_t bytes);
    Payload materialize(PayloadPoolReservation&& reservation, PayloadView source);
    PayloadStats stats() const;
    std::size_t block_size_bytes() const noexcept;
    std::size_t block_count() const noexcept;
    std::size_t capacity_bytes() const noexcept;
    std::size_t used_bytes() const noexcept;
    std::size_t available_bytes() const noexcept;
    std::size_t high_water_bytes() const noexcept;
    void release(std::size_t class_index, std::size_t block_index) noexcept;
    void set_capacity_changed_sink(std::function<void()> sink);

private:
    struct ClassState {
        std::size_t max_payload_bytes = 0;
        std::size_t block_count = 0;
        std::unique_ptr<std::uint8_t[]> storage;
        std::vector<std::size_t> free_blocks;
        std::atomic<std::size_t> used_blocks{0};
        std::atomic<std::size_t> high_water_blocks{0};
        std::atomic<std::uint64_t> exhaustion_count{0};
    };

    std::size_t select_class_locked(std::size_t bytes) const noexcept;

    std::vector<std::unique_ptr<ClassState>> classes_;
    mutable std::mutex mutex_;
    std::function<void()> capacity_changed_sink_;
};

}  // namespace rodaq

#endif  // RODAQ_PAYLOAD_POOL_STATE_HPP_
