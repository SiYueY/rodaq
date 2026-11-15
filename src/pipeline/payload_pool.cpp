#include "payload_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <mutex>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace rodaq::pipeline {

struct PayloadPoolState {
    const PayloadPoolOptions options;
    std::unique_ptr<std::byte[]> bytes;
    // Metadata is a fixed Prepare cost, separate from per-payload backing charge.
    std::vector<std::uint8_t> allocated_pages;
    mutable std::mutex mutex;
    PayloadPoolSnapshot counters;

    explicit PayloadPoolState(PayloadPoolOptions policy)
    : options(policy),
      bytes(std::make_unique<std::byte[]>(policy.physical_bytes)),
      allocated_pages(policy.physical_bytes / policy.page_bytes, 0) {
        counters.capacity_bytes = policy.physical_bytes;
    }
};

Payload::Payload(
    std::shared_ptr<PayloadPoolState> state, std::size_t first_page, std::size_t pages,
    std::size_t logical_size) noexcept
: state_(std::move(state)), first_page_(first_page), pages_(pages), logical_size_(logical_size) {}

Payload::Payload(Payload&& other) noexcept
: state_(std::move(other.state_)),
  first_page_(std::exchange(other.first_page_, 0)),
  pages_(std::exchange(other.pages_, 0)),
  logical_size_(std::exchange(other.logical_size_, 0)) {}

Payload& Payload::operator=(Payload&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        first_page_ = std::exchange(other.first_page_, 0);
        pages_ = std::exchange(other.pages_, 0);
        logical_size_ = std::exchange(other.logical_size_, 0);
    }
    return *this;
}

Payload::~Payload() noexcept { reset(); }

Payload::operator bool() const noexcept { return state_ != nullptr; }

const std::byte* Payload::data() const noexcept {
    return state_ && logical_size_ != 0
               ? state_->bytes.get() + first_page_ * state_->options.page_bytes
               : nullptr;
}

std::size_t Payload::size() const noexcept { return logical_size_; }

std::size_t Payload::physical_charge() const noexcept {
    return state_ ? pages_ * state_->options.page_bytes : 0;
}

Result<void> Payload::copy_from(const void* source, std::size_t size) noexcept {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    if (size != logical_size_ || (source == nullptr && size != 0)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (size != 0) {
        std::memcpy(state_->bytes.get() + first_page_ * state_->options.page_bytes, source, size);
    }
    return {};
}

void Payload::reset() noexcept {
    if (!state_) {
        return;
    }
    auto state = std::move(state_);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        assert(state->counters.live_buffers != 0);
        assert(state->counters.used_physical_bytes >= pages_ * state->options.page_bytes);
        for (std::size_t index = first_page_; index < first_page_ + pages_; ++index) {
            assert(state->allocated_pages[index] != 0);
            state->allocated_pages[index] = 0;
        }
        --state->counters.live_buffers;
        state->counters.used_physical_bytes -= pages_ * state->options.page_bytes;
    }
    first_page_ = 0;
    pages_ = 0;
    logical_size_ = 0;
    // Last-reference arena destruction occurs outside the pool accounting lock.
}

PayloadPool::PayloadPool(std::shared_ptr<PayloadPoolState> state) noexcept
: state_(std::move(state)) {}

Result<PayloadPool> PayloadPool::create(PayloadPoolOptions options) {
    if (options.page_bytes < 256 || options.page_bytes > 1024 * 1024 ||
        (options.page_bytes & (options.page_bytes - 1)) != 0 || options.physical_bytes == 0 ||
        options.physical_bytes % options.page_bytes != 0 ||
        options.physical_bytes / options.page_bytes > 65536 || options.max_payload_bytes == 0 ||
        options.max_payload_bytes > options.physical_bytes ||
        options.max_payload_bytes > std::numeric_limits<std::uint32_t>::max() ||
        options.max_buffers == 0 || options.max_buffers > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        return PayloadPool(std::make_shared<PayloadPoolState>(options));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<Payload> PayloadPool::reserve(std::uint64_t logical_size) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    if (logical_size > state_->options.max_payload_bytes) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto size = static_cast<std::size_t>(logical_size);
    const auto page_bytes = state_->options.page_bytes;
    const auto pages = size / page_bytes + (size % page_bytes != 0 ? 1 : 0);
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& counters = state_->counters;
    if (counters.live_buffers == state_->options.max_buffers ||
        pages > (counters.capacity_bytes - counters.used_physical_bytes) / page_bytes) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    std::size_t first = 0;
    if (pages != 0) {
        std::size_t run = 0;
        bool found = false;
        for (std::size_t index = 0; index < state_->allocated_pages.size(); ++index) {
            run = state_->allocated_pages[index] == 0 ? run + 1 : 0;
            if (run == pages) {
                first = index + 1 - pages;
                found = true;
                break;
            }
        }
        if (!found) {
            return Status(StatusCode::ResourceExhausted, {});
        }
    }
    for (std::size_t index = first; index < first + pages; ++index) {
        state_->allocated_pages[index] = 1;
    }
    ++counters.live_buffers;
    counters.used_physical_bytes += pages * page_bytes;
    counters.high_water_buffers = std::max(counters.high_water_buffers, counters.live_buffers);
    counters.high_water_physical_bytes =
        std::max(counters.high_water_physical_bytes, counters.used_physical_bytes);
    return Payload(state_, first, pages, size);
}

PayloadPoolSnapshot PayloadPool::snapshot() const {
    if (!state_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->counters;
}

}  // namespace rodaq::pipeline
