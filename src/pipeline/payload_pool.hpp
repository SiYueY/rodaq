#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <rodaq/result.hpp>

namespace rodaq::pipeline {

struct PayloadPoolOptions {
    std::size_t physical_bytes{1024 * 1024};
    std::size_t page_bytes{4096};
    std::size_t max_payload_bytes{1024 * 1024};
    std::uint32_t max_buffers{256};
};

struct PayloadPoolSnapshot {
    std::size_t capacity_bytes{0};
    std::size_t used_physical_bytes{0};
    std::size_t high_water_physical_bytes{0};
    std::uint32_t live_buffers{0};
    std::uint32_t high_water_buffers{0};
};

struct PayloadPoolState;
class PayloadPool;

// Core-owned COPY backing. Release is memory-only and exactly once, including
// rollback before Admission. Shared backing outlives the pool facade whenever
// a queued Record or an actual in-flight Storage call still owns its payload.
class Payload {
public:
    Payload() noexcept = default;
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    Payload(Payload&& other) noexcept;
    Payload& operator=(Payload&& other) noexcept;
    ~Payload() noexcept;

    explicit operator bool() const noexcept;
    const std::byte* data() const noexcept;
    std::size_t size() const noexcept;
    std::size_t physical_charge() const noexcept;
    Result<void> copy_from(const void* source, std::size_t size) noexcept;
    void reset() noexcept;

private:
    friend class PayloadPool;
    Payload(
        std::shared_ptr<PayloadPoolState> state, std::size_t first_page, std::size_t pages,
        std::size_t logical_size) noexcept;

    std::shared_ptr<PayloadPoolState> state_;
    std::size_t first_page_{0};
    std::size_t pages_{0};
    std::size_t logical_size_{0};
};

// Fixed backing and allocation metadata are prepared once. A bounded first-fit
// page scan may reject fragmented capacity before Admission; it never grows the
// arena or waits for downstream progress. No allocation/copy runs under Gate.
class PayloadPool {
public:
    static Result<PayloadPool> create(PayloadPoolOptions options);
    PayloadPool(const PayloadPool&) = delete;
    PayloadPool& operator=(const PayloadPool&) = delete;
    PayloadPool(PayloadPool&&) noexcept = default;
    PayloadPool& operator=(PayloadPool&&) noexcept = default;

    Result<Payload> reserve(std::uint64_t logical_size);
    PayloadPoolSnapshot snapshot() const;

private:
    explicit PayloadPool(std::shared_ptr<PayloadPoolState> state) noexcept;
    std::shared_ptr<PayloadPoolState> state_;
};

}  // namespace rodaq::pipeline
