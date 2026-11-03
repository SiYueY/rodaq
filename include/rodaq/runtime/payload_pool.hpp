#ifndef RODAQ_RUNTIME_PAYLOAD_POOL_HPP_
#define RODAQ_RUNTIME_PAYLOAD_POOL_HPP_

#include <rodaq/result.hpp>
#include <rodaq/runtime/payload.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rodaq {

/// A bounded pool of reusable Payload blocks.
///
/// The pool never falls back to heap allocation when acquiring a Payload.
/// Acquire operations are thread-safe.
class PayloadPool final {
public:
    static Result<PayloadPool> Create(std::size_t block_size_bytes, std::size_t block_count);

    PayloadPool(const PayloadPool&) = delete;
    PayloadPool& operator=(const PayloadPool&) = delete;
    PayloadPool(PayloadPool&&) noexcept = default;
    PayloadPool& operator=(PayloadPool&&) noexcept = default;
    ~PayloadPool() = default;

    Result<Payload> Acquire(std::size_t bytes);
    Result<Payload> Acquire(std::size_t bytes, PayloadView source);

    std::size_t BlockSizeBytes() const noexcept;
    std::size_t BlockCount() const noexcept;
    std::size_t CapacityBytes() const noexcept;
    std::size_t UsedBytes() const noexcept;
    std::size_t AvailableBytes() const noexcept;
    std::size_t HighWaterBytes() const noexcept;

private:
    explicit PayloadPool(std::shared_ptr<PayloadPoolState> state) noexcept;

    std::shared_ptr<PayloadPoolState> state_;
};

}  // namespace rodaq

#endif  // RODAQ_RUNTIME_PAYLOAD_POOL_HPP_
