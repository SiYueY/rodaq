#include "record_budget.hpp"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <new>
#include <utility>

namespace rodaq::pipeline {

struct RecordBudgetState {
    std::mutex mutex;
    std::uint64_t max_records{0};
    std::uint64_t max_bytes{0};
    RecordBudgetSnapshot counters;
};

RecordCharge::RecordCharge(std::shared_ptr<RecordBudgetState> state, std::uint64_t bytes) noexcept
: state_(std::move(state)), bytes_(bytes) {}

RecordCharge::RecordCharge(RecordCharge&& other) noexcept
: state_(std::move(other.state_)), bytes_(std::exchange(other.bytes_, 0)) {}

RecordCharge& RecordCharge::operator=(RecordCharge&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

RecordCharge::~RecordCharge() noexcept { reset(); }

RecordCharge::operator bool() const noexcept { return state_ != nullptr; }

std::uint64_t RecordCharge::logical_bytes() const noexcept { return bytes_; }

void RecordCharge::reset() noexcept {
    if (state_) {
        auto state = std::move(state_);
        std::lock_guard<std::mutex> lock(state->mutex);
        assert(state->counters.records != 0 && state->counters.logical_bytes >= bytes_);
        --state->counters.records;
        state->counters.logical_bytes -= bytes_;
    }
    bytes_ = 0;
}

RecordBudget::RecordBudget(std::shared_ptr<RecordBudgetState> state) noexcept
: state_(std::move(state)) {}

Result<RecordBudget> RecordBudget::create(
    std::uint64_t max_records, std::uint64_t max_logical_bytes) {
    if (max_records == 0 || max_logical_bytes == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_shared<RecordBudgetState>();
        state->max_records = max_records;
        state->max_bytes = max_logical_bytes;
        return RecordBudget(std::move(state));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<RecordCharge> RecordBudget::reserve(std::uint64_t logical_bytes) {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& current = state_->counters;
    if (current.records == state_->max_records ||
        logical_bytes > state_->max_bytes - current.logical_bytes) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    ++current.records;
    current.logical_bytes += logical_bytes;
    current.high_water_records = std::max(current.high_water_records, current.records);
    current.high_water_logical_bytes =
        std::max(current.high_water_logical_bytes, current.logical_bytes);
    return RecordCharge(state_, logical_bytes);
}

RecordBudgetSnapshot RecordBudget::snapshot() const {
    if (!state_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->counters;
}

}  // namespace rodaq::pipeline
