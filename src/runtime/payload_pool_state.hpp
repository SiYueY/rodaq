#ifndef RODAQ_RUNTIME_PAYLOAD_POOL_STATE_HPP_
#define RODAQ_RUNTIME_PAYLOAD_POOL_STATE_HPP_

#include <rodaq/runtime/payload.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace rodaq {

class PayloadPoolState final : public std::enable_shared_from_this<PayloadPoolState> {
public:
    PayloadPoolState(std::size_t block_size_bytes, std::size_t block_count);

    PayloadPoolState(const PayloadPoolState&) = delete;
    PayloadPoolState& operator=(const PayloadPoolState&) = delete;
    PayloadPoolState(PayloadPoolState&&) = delete;
    PayloadPoolState& operator=(PayloadPoolState&&) = delete;

    Payload Acquire(std::size_t bytes);

    std::size_t BlockSizeBytes() const noexcept { return block_size_bytes_; }

    std::size_t BlockCount() const noexcept { return block_count_; }

    std::size_t CapacityBytes() const noexcept { return capacity_bytes_; }

    std::size_t UsedBytes() const;

    std::size_t AvailableBytes() const;

    std::size_t HighWaterBytes() const;

    void Release(std::size_t block_index) noexcept;

private:
    std::size_t block_size_bytes_;
    std::size_t block_count_;
    std::size_t capacity_bytes_;
    std::unique_ptr<std::uint8_t[]> storage_;
    std::vector<std::size_t> free_blocks_;
    mutable std::mutex mutex_;
    std::atomic<std::size_t> used_blocks_{0};
    std::atomic<std::size_t> high_water_blocks_{0};
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_PAYLOAD_POOL_STATE_HPP_
