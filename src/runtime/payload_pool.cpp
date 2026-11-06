#include "rodaq/runtime/payload_pool.hpp"

#include "payload_pool_state.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace rodaq {

PayloadPoolState::PayloadPoolState(const PayloadPoolConfig& config) {
    if (config.classes.empty()) {
        throw std::invalid_argument("PayloadPool requires at least one size class");
    }

    classes_.reserve(config.classes.size());
    std::size_t previous_size = 0;
    for (const PayloadClassConfig& class_config : config.classes) {
        if (class_config.max_payload_bytes == 0 || class_config.block_count == 0 ||
            class_config.max_payload_bytes <= previous_size ||
            class_config.max_payload_bytes >
                std::numeric_limits<std::size_t>::max() / class_config.block_count) {
            throw std::invalid_argument("PayloadPool size class configuration is invalid");
        }

        auto state = std::make_unique<ClassState>();
        state->max_payload_bytes = class_config.max_payload_bytes;
        state->block_count = class_config.block_count;
        const std::size_t physical_bytes =
            class_config.max_payload_bytes * class_config.block_count;
        state->storage = std::make_unique<std::uint8_t[]>(physical_bytes);
        state->free_blocks.reserve(class_config.block_count);
        for (std::size_t offset = 0; offset < class_config.block_count; ++offset) {
            state->free_blocks.push_back(class_config.block_count - offset - 1U);
        }
        classes_.push_back(std::move(state));
        previous_size = class_config.max_payload_bytes;
    }
}

PayloadPoolReservation::PayloadPoolReservation(
    std::shared_ptr<PayloadPoolState> state, std::size_t class_index, std::size_t block_index,
    std::size_t payload_size) noexcept
: state_(std::move(state)),
  class_index_(class_index),
  block_index_(block_index),
  payload_size_(payload_size) {}

PayloadPoolReservation::PayloadPoolReservation(PayloadPoolReservation&& other) noexcept
: state_(std::move(other.state_)),
  class_index_(other.class_index_),
  block_index_(other.block_index_),
  payload_size_(other.payload_size_) {
    other.class_index_ = 0;
    other.block_index_ = 0;
    other.payload_size_ = 0;
}

PayloadPoolReservation& PayloadPoolReservation::operator=(PayloadPoolReservation&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        class_index_ = other.class_index_;
        block_index_ = other.block_index_;
        payload_size_ = other.payload_size_;
        other.class_index_ = 0;
        other.block_index_ = 0;
        other.payload_size_ = 0;
    }
    return *this;
}

PayloadPoolReservation::~PayloadPoolReservation() { release(); }

bool PayloadPoolReservation::active() const noexcept { return state_ != nullptr; }

std::size_t PayloadPoolReservation::class_index() const noexcept { return class_index_; }

std::size_t PayloadPoolReservation::block_index() const noexcept { return block_index_; }

void PayloadPoolReservation::release() noexcept {
    if (state_ != nullptr) {
        std::shared_ptr<PayloadPoolState> state = std::move(state_);
        const std::size_t class_index = class_index_;
        const std::size_t block_index = block_index_;
        class_index_ = 0;
        block_index_ = 0;
        payload_size_ = 0;
        state->release(class_index, block_index);
    }
}

std::size_t PayloadPoolState::select_class_locked(std::size_t bytes) const noexcept {
    for (std::size_t index = 0; index < classes_.size(); ++index) {
        if (bytes <= classes_[index]->max_payload_bytes) {
            return index;
        }
    }
    return classes_.size();
}

Result<PayloadPoolReservation> PayloadPoolState::try_reserve(std::size_t bytes) {
    if (bytes == 0) {
        return PayloadPoolReservation();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t class_index = select_class_locked(bytes);
    if (class_index >= classes_.size()) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "payload exceeds all configured Payload size classes");
    }

    ClassState& size_class = *classes_[class_index];
    if (size_class.free_blocks.empty()) {
        size_class.exhaustion_count.fetch_add(1, std::memory_order_relaxed);
        return rodaq::Status(
            StatusCode::ResourceExhausted, "payload size class has no available blocks");
    }

    const std::size_t block_index = size_class.free_blocks.back();
    size_class.free_blocks.pop_back();
    const std::size_t used = size_class.used_blocks.fetch_add(1, std::memory_order_relaxed) + 1U;
    std::size_t high_water = size_class.high_water_blocks.load(std::memory_order_relaxed);
    while (used > high_water && !size_class.high_water_blocks.compare_exchange_weak(
                                    high_water, used, std::memory_order_relaxed)) {
    }
    return PayloadPoolReservation(shared_from_this(), class_index, block_index, bytes);
}

Payload PayloadPoolState::materialize(PayloadPoolReservation&& reservation, PayloadView source) {
    if (!reservation.active() || reservation.state_.get() != this ||
        source.size() != reservation.payload_size_ || !source.valid() ||
        reservation.class_index_ >= classes_.size()) {
        return Payload();
    }

    ClassState& size_class = *classes_[reservation.class_index_];
    std::uint8_t* destination =
        size_class.storage.get() + reservation.block_index_ * size_class.max_payload_bytes;
    if (source.size() != 0) {
        std::memcpy(destination, source.data(), source.size());
    }

    Payload payload(
        std::move(reservation.state_), destination, source.size(), reservation.class_index_,
        reservation.block_index_);
    reservation.class_index_ = 0;
    reservation.block_index_ = 0;
    reservation.payload_size_ = 0;
    return payload;
}

void PayloadPoolState::release(std::size_t class_index, std::size_t block_index) noexcept {
    std::function<void()> sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (class_index >= classes_.size() || block_index >= classes_[class_index]->block_count) {
            return;
        }
        ClassState& size_class = *classes_[class_index];
        size_class.free_blocks.push_back(block_index);
        size_class.used_blocks.fetch_sub(1, std::memory_order_relaxed);
        sink = capacity_changed_sink_;
    }
    if (sink) {
        sink();
    }
}

PayloadPoolStats PayloadPoolState::stats() const {
    PayloadPoolStats stats;
    stats.classes.reserve(classes_.size());
    for (const std::unique_ptr<ClassState>& class_state : classes_) {
        const ClassState& size_class = *class_state;
        stats.classes.push_back(PayloadClassStats{
            size_class.max_payload_bytes,
            size_class.block_count,
            size_class.used_blocks.load(std::memory_order_relaxed),
            size_class.block_count - size_class.used_blocks.load(std::memory_order_relaxed),
            size_class.high_water_blocks.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(size_class.max_payload_bytes) *
                size_class.used_blocks.load(std::memory_order_relaxed),
            static_cast<std::uint64_t>(size_class.max_payload_bytes) *
                size_class.high_water_blocks.load(std::memory_order_relaxed),
            size_class.exhaustion_count.load(std::memory_order_relaxed),
        });
    }
    return stats;
}

std::size_t PayloadPoolState::block_size_bytes() const noexcept {
    return classes_.empty() ? 0 : classes_.back()->max_payload_bytes;
}

std::size_t PayloadPoolState::block_count() const noexcept {
    std::size_t count = 0;
    for (const auto& size_class : classes_) {
        count += size_class->block_count;
    }
    return count;
}

std::size_t PayloadPoolState::capacity_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& size_class : classes_) {
        bytes += size_class->max_payload_bytes * size_class->block_count;
    }
    return bytes;
}

std::size_t PayloadPoolState::used_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& size_class : classes_) {
        bytes +=
            size_class->max_payload_bytes * size_class->used_blocks.load(std::memory_order_relaxed);
    }
    return bytes;
}

std::size_t PayloadPoolState::available_bytes() const noexcept {
    return capacity_bytes() - used_bytes();
}

std::size_t PayloadPoolState::high_water_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& size_class : classes_) {
        bytes += size_class->max_payload_bytes *
                 size_class->high_water_blocks.load(std::memory_order_relaxed);
    }
    return bytes;
}

void PayloadPoolState::set_capacity_changed_sink(std::function<void()> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_changed_sink_ = std::move(sink);
}

Result<PayloadPool> PayloadPool::create(const PayloadPoolConfig& config) {
    try {
        return PayloadPool(std::make_shared<PayloadPoolState>(config));
    } catch (const std::bad_alloc&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "failed to allocate payload pool storage");
    } catch (const std::length_error&) {
        return rodaq::Status(
            StatusCode::ResourceExhausted, "payload pool size exceeds container limits");
    } catch (const std::exception& error) {
        return rodaq::Status(StatusCode::InvalidArgument, error.what());
    }
}

Result<PayloadPool> PayloadPool::create(std::size_t block_size_bytes, std::size_t block_count) {
    PayloadPoolConfig config;
    config.classes.push_back(PayloadClassConfig{block_size_bytes, block_count});
    return create(config);
}

Result<PayloadPoolReservation> PayloadPool::try_reserve(std::size_t bytes) {
    return state_->try_reserve(bytes);
}

Result<Payload> PayloadPool::materialize(PayloadPoolReservation&& reservation, PayloadView source) {
    if (!reservation.active()) {
        if (source.size() == 0 && source.valid()) {
            return Payload();
        }
        return rodaq::Status(StatusCode::InvalidArgument, "payload reservation is not active");
    }
    if (!source.valid()) {
        return rodaq::Status(StatusCode::InvalidArgument, "payload source view is invalid");
    }
    const std::size_t expected = reservation.payload_size_;
    if (source.size() != expected) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "payload source size does not match reservation");
    }
    return state_->materialize(std::move(reservation), source);
}

Result<Payload> PayloadPool::acquire(std::size_t bytes, PayloadView source) {
    if (source.size() != bytes) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "payload source size does not match the requested size");
    }
    if (!source.valid()) {
        return rodaq::Status(
            StatusCode::InvalidArgument, "payload source view is structurally invalid");
    }
    auto reserved = try_reserve(bytes);
    if (!reserved.ok()) {
        return std::move(reserved).status();
    }
    if (bytes == 0) {
        return Payload();
    }
    return state_->materialize(std::move(reserved.value()), source);
}

PayloadPoolStats PayloadPool::stats() const { return state_->stats(); }

std::size_t PayloadPool::block_size_bytes() const noexcept { return state_->block_size_bytes(); }

std::size_t PayloadPool::block_count() const noexcept { return state_->block_count(); }

std::size_t PayloadPool::capacity_bytes() const noexcept { return state_->capacity_bytes(); }

std::size_t PayloadPool::used_bytes() const noexcept { return state_->used_bytes(); }

std::size_t PayloadPool::available_bytes() const noexcept { return state_->available_bytes(); }

std::size_t PayloadPool::high_water_bytes() const noexcept { return state_->high_water_bytes(); }

void PayloadPool::set_capacity_changed_sink(std::function<void()> sink) {
    state_->set_capacity_changed_sink(std::move(sink));
}

PayloadPool::PayloadPool(std::shared_ptr<PayloadPoolState> state) noexcept
: state_(std::move(state)) {}

}  // namespace rodaq
