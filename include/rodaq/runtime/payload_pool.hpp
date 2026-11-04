#ifndef RODAQ_RUNTIME_PAYLOAD_POOL_HPP_
#define RODAQ_RUNTIME_PAYLOAD_POOL_HPP_

#include <rodaq/result.hpp>
#include <rodaq/runtime/payload.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rodaq {

class PayloadPoolState;

struct PayloadClassConfig {
    std::size_t max_payload_bytes = 0;
    std::size_t block_count = 0;
};

struct PayloadPoolConfig {
    std::vector<PayloadClassConfig> classes;
};

struct PayloadClassStats {
    std::size_t max_payload_bytes = 0;
    std::size_t block_count = 0;
    std::size_t used_blocks = 0;
    std::size_t available_blocks = 0;
    std::size_t high_water_blocks = 0;
    std::uint64_t current_physical_bytes = 0;
    std::uint64_t high_water_physical_bytes = 0;
    std::uint64_t exhaustion_count = 0;
};

struct PayloadPoolStats {
    std::vector<PayloadClassStats> classes;
};

/// A pre-Gate physical block reservation from one Payload size class.
class PayloadPoolReservation final {
public:
    PayloadPoolReservation() noexcept = default;
    PayloadPoolReservation(const PayloadPoolReservation&) = delete;
    PayloadPoolReservation& operator=(const PayloadPoolReservation&) = delete;
    PayloadPoolReservation(PayloadPoolReservation&& other) noexcept;
    PayloadPoolReservation& operator=(PayloadPoolReservation&& other) noexcept;
    ~PayloadPoolReservation();

    bool Active() const noexcept;
    std::size_t ClassIndex() const noexcept;
    std::size_t BlockIndex() const noexcept;
    void Release() noexcept;

private:
    friend class PayloadPool;
    friend class PayloadPoolState;

    PayloadPoolReservation(
        std::shared_ptr<PayloadPoolState> state, std::size_t class_index, std::size_t block_index,
        std::size_t payload_size) noexcept;

    std::shared_ptr<PayloadPoolState> state_;
    std::size_t class_index_ = 0;
    std::size_t block_index_ = 0;
    std::size_t payload_size_ = 0;
};

/// A bounded, activation-lifetime Copy Payload pool with isolated size classes.
class PayloadPool final {
public:
    static Result<PayloadPool> Create(std::size_t block_size_bytes, std::size_t block_count);
    static Result<PayloadPool> Create(const PayloadPoolConfig& config);

    PayloadPool(const PayloadPool&) = delete;
    PayloadPool& operator=(const PayloadPool&) = delete;
    PayloadPool(PayloadPool&&) noexcept = default;
    PayloadPool& operator=(PayloadPool&&) noexcept = default;
    ~PayloadPool() = default;

    Result<PayloadPoolReservation> TryReserve(std::size_t bytes);
    Result<Payload> Materialize(PayloadPoolReservation&& reservation, PayloadView source);
    Result<Payload> Acquire(std::size_t bytes, PayloadView source);

    PayloadPoolStats Stats() const;
    std::size_t BlockSizeBytes() const noexcept;
    std::size_t BlockCount() const noexcept;
    std::size_t CapacityBytes() const noexcept;
    std::size_t UsedBytes() const noexcept;
    std::size_t AvailableBytes() const noexcept;
    std::size_t HighWaterBytes() const noexcept;
    void SetCapacityChangedSink(std::function<void()> sink);

private:
    explicit PayloadPool(std::shared_ptr<PayloadPoolState> state) noexcept;

    std::shared_ptr<PayloadPoolState> state_;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_PAYLOAD_POOL_HPP_
