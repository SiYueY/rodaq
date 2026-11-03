#include "rodaq/runtime/payload.hpp"

#include "payload_pool_state.hpp"

#include <utility>

namespace rodaq {

Payload::Payload(
    std::shared_ptr<PayloadPoolState> pool_state, std::uint8_t* data, std::size_t size,
    std::size_t block_index) noexcept
: pool_state_(std::move(pool_state)), data_(data), size_(size), block_index_(block_index) {}

Payload::Payload(Payload&& other) noexcept
: pool_state_(std::move(other.pool_state_)),
  data_(other.data_),
  size_(other.size_),
  block_index_(other.block_index_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.block_index_ = 0;
}

Payload& Payload::operator=(Payload&& other) noexcept {
    if (this != &other) {
        ReleaseCurrent();
        pool_state_ = std::move(other.pool_state_);
        data_ = other.data_;
        size_ = other.size_;
        block_index_ = other.block_index_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.block_index_ = 0;
    }
    return *this;
}

Payload::~Payload() { ReleaseCurrent(); }

PayloadView Payload::View() const noexcept { return PayloadView(data_, size_); }

std::size_t Payload::Size() const noexcept { return size_; }

bool Payload::Empty() const noexcept { return size_ == 0; }

void Payload::ReleaseCurrent() noexcept {
    if (pool_state_ != nullptr) {
        pool_state_->Release(block_index_);
        pool_state_.reset();
    }
    data_ = nullptr;
    size_ = 0;
    block_index_ = 0;
}

}  // namespace rodaq
