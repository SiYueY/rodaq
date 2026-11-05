#include "rodaq/runtime/payload.hpp"

#include "payload_pool_state.hpp"

#include <utility>

namespace rodaq {

Result<Payload> Payload::adopt(PayloadView view, void* owner_context, PayloadRelease release) {
    if (!view.valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "adopted payload view is structurally invalid"));
    }
    if (release == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "adopted payload requires a release capability"));
    }
    return Payload(view.data(), view.size(), owner_context, release);
}

Result<Payload> Payload::retain(PayloadView view, void* owner_context, PayloadRelease release) {
    if (!view.valid()) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "retained payload view is structurally invalid"));
    }
    if (release == nullptr) {
        return nonstd::make_unexpected(
            Error(ErrorCode::InvalidArgument, "retained payload requires a release capability"));
    }
    return Payload(view.data(), view.size(), owner_context, release);
}

Payload::Payload(
    std::shared_ptr<PayloadPoolState> pool_state, std::uint8_t* data, std::size_t size,
    std::size_t class_index, std::size_t block_index) noexcept
: pool_state_(std::move(pool_state)),
  data_(data),
  size_(size),
  block_class_(class_index),
  block_index_(block_index) {}

Payload::Payload(
    const std::uint8_t* data, std::size_t size, void* owner_context,
    PayloadRelease release) noexcept
: data_(const_cast<std::uint8_t*>(data)),
  size_(size),
  owner_context_(owner_context),
  release_(release) {}

Payload::Payload(Payload&& other) noexcept
: pool_state_(std::move(other.pool_state_)),
  data_(other.data_),
  size_(other.size_),
  block_class_(other.block_class_),
  block_index_(other.block_index_),
  owner_context_(other.owner_context_),
  release_(other.release_),
  lifetime_guard_(std::move(other.lifetime_guard_)) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.block_class_ = 0;
    other.block_index_ = 0;
    other.owner_context_ = nullptr;
    other.release_ = nullptr;
}

Payload& Payload::operator=(Payload&& other) noexcept {
    if (this != &other) {
        release_current();
        pool_state_ = std::move(other.pool_state_);
        data_ = other.data_;
        size_ = other.size_;
        block_class_ = other.block_class_;
        block_index_ = other.block_index_;
        owner_context_ = other.owner_context_;
        release_ = other.release_;
        lifetime_guard_ = std::move(other.lifetime_guard_);
        other.data_ = nullptr;
        other.size_ = 0;
        other.block_class_ = 0;
        other.block_index_ = 0;
        other.owner_context_ = nullptr;
        other.release_ = nullptr;
    }
    return *this;
}

Payload::~Payload() { release_current(); }

PayloadView Payload::view() const noexcept { return PayloadView(data_, size_); }

std::size_t Payload::size() const noexcept { return size_; }

bool Payload::empty() const noexcept { return size_ == 0; }

bool Payload::requires_external_release() const noexcept { return release_ != nullptr; }

void Payload::attach_lifetime_guard(std::shared_ptr<void> lifetime_guard) noexcept {
    lifetime_guard_ = std::move(lifetime_guard);
}

void Payload::release_current() noexcept {
    if (pool_state_ != nullptr) {
        pool_state_->release(block_class_, block_index_);
        pool_state_.reset();
    }
    if (release_ != nullptr) {
        void* owner_context = owner_context_;
        PayloadRelease release = release_;
        owner_context_ = nullptr;
        release_ = nullptr;
        release(owner_context);
    }
    data_ = nullptr;
    size_ = 0;
    block_class_ = 0;
    block_index_ = 0;
}

}  // namespace rodaq
