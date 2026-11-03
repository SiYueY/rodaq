#include "rodaq/runtime/payload_pool.hpp"

#include "payload_pool_state.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace rodaq {

PayloadPoolState::PayloadPoolState(std::size_t block_size_bytes, std::size_t block_count)
: block_size_bytes_(block_size_bytes),
  block_count_(block_count),
  capacity_bytes_(
      block_size_bytes > std::numeric_limits<std::size_t>::max() / block_count
          ? 0
          : block_size_bytes * block_count),
  storage_(std::make_unique<std::uint8_t[]>(capacity_bytes_)),
  free_blocks_() {
    free_blocks_.reserve(block_count_);
    for (std::size_t offset = 0; offset < block_count_; ++offset) {
        free_blocks_.push_back(block_count_ - offset - 1U);
    }
}

Payload PayloadPoolState::Acquire(std::size_t bytes) {
    if (bytes == 0) {
        return Payload();
    }
    if (bytes > block_size_bytes_) {
        return Payload();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (free_blocks_.empty()) {
        return Payload();
    }

    const std::size_t block_index = free_blocks_.back();
    free_blocks_.pop_back();
    const std::size_t used_blocks = used_blocks_.fetch_add(1) + 1U;
    std::size_t high_water_blocks = high_water_blocks_.load();
    while (used_blocks > high_water_blocks &&
           !high_water_blocks_.compare_exchange_weak(high_water_blocks, used_blocks)) {
    }
    return Payload(
        shared_from_this(), storage_.get() + (block_index * block_size_bytes_), bytes, block_index);
}

std::size_t PayloadPoolState::UsedBytes() const { return used_blocks_.load() * block_size_bytes_; }

std::size_t PayloadPoolState::AvailableBytes() const {
    return (block_count_ - used_blocks_.load()) * block_size_bytes_;
}

std::size_t PayloadPoolState::HighWaterBytes() const {
    return high_water_blocks_.load() * block_size_bytes_;
}

void PayloadPoolState::Release(std::size_t block_index) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    free_blocks_.push_back(block_index);
    used_blocks_.fetch_sub(1);
}

Result<PayloadPool> PayloadPool::Create(std::size_t block_size_bytes, std::size_t block_count) {
    if (block_size_bytes == 0 || block_count == 0) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "payload pool block size and count must be non-zero"));
    }
    if (block_size_bytes > std::numeric_limits<std::size_t>::max() / block_count) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "payload pool byte capacity overflows"));
    }

    try {
        return PayloadPool(std::make_shared<PayloadPoolState>(block_size_bytes, block_count));
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "failed to allocate payload pool storage"));
    } catch (const std::length_error&) {
        return nonstd::make_unexpected(Error(
            ErrorCode::ResourceExhausted, "payload pool block count exceeds container limits"));
    }
}

Result<Payload> PayloadPool::Acquire(std::size_t bytes) {
    if (bytes > state_->BlockSizeBytes()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "requested payload exceeds the pool block size"));
    }
    if (bytes == 0) {
        return Payload();
    }

    Payload payload = state_->Acquire(bytes);
    if (payload.Empty()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::ResourceExhausted, "payload pool has no available blocks"));
    }
    return payload;
}

Result<Payload> PayloadPool::Acquire(std::size_t bytes, PayloadView source) {
    if (source.Size() != bytes) {
        return nonstd::make_unexpected(Error(
            ErrorCode::InvalidArgument, "payload source size does not match the requested size"));
    }

    auto payload = Acquire(bytes);
    if (!payload.has_value()) {
        return nonstd::make_unexpected(payload.error());
    }
    if (bytes != 0) {
        const std::uint8_t* source_data = source.Data();
        std::uint8_t* destination = payload.value().data_;
        for (std::size_t index = 0; index < bytes; ++index) {
            destination[index] = source_data[index];
        }
    }
    return payload;
}

std::size_t PayloadPool::BlockSizeBytes() const noexcept { return state_->BlockSizeBytes(); }

std::size_t PayloadPool::BlockCount() const noexcept { return state_->BlockCount(); }

std::size_t PayloadPool::CapacityBytes() const noexcept { return state_->CapacityBytes(); }

std::size_t PayloadPool::UsedBytes() const noexcept { return state_->UsedBytes(); }

std::size_t PayloadPool::AvailableBytes() const noexcept { return state_->AvailableBytes(); }

std::size_t PayloadPool::HighWaterBytes() const noexcept { return state_->HighWaterBytes(); }

PayloadPool::PayloadPool(std::shared_ptr<PayloadPoolState> state) noexcept
: state_(std::move(state)) {}

}  // namespace rodaq
