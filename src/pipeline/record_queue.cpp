#include "record_queue.hpp"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace rodaq::pipeline {

struct RecordQueueState {
    std::mutex mutex;
    std::vector<std::optional<Record>> slots;
    std::size_t read_position{0};
    std::size_t write_position{0};
    RecordQueueSnapshot counters;
};

QueueReservation::QueueReservation(std::shared_ptr<RecordQueueState> state) noexcept
: state_(std::move(state)) {}

QueueReservation::QueueReservation(QueueReservation&& other) noexcept
: state_(std::move(other.state_)) {}

QueueReservation& QueueReservation::operator=(QueueReservation&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
    }
    return *this;
}

QueueReservation::~QueueReservation() noexcept { release(); }

void QueueReservation::release() noexcept {
    if (state_) {
        auto state = std::move(state_);
        std::lock_guard<std::mutex> lock(state->mutex);
        assert(state->counters.reserved_slots != 0);
        --state->counters.reserved_slots;
    }
}

RecordQueue::RecordQueue(std::shared_ptr<RecordQueueState> state) noexcept
: state_(std::move(state)) {}

Result<RecordQueue> RecordQueue::create(std::size_t capacity) {
    if (capacity == 0 || capacity > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_shared<RecordQueueState>();
        state->slots.resize(capacity);
        state->counters.capacity = capacity;
        state->counters.accepting = true;
        return RecordQueue(std::move(state));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<QueueReservation> RecordQueue::reserve() {
    if (!state_) {
        return Status(StatusCode::InvalidState, {});
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto& counters = state_->counters;
    if (!counters.accepting) {
        return Status(StatusCode::InvalidState, {});
    }
    if (counters.queued_records + counters.reserved_slots == counters.capacity) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    ++state_->counters.reserved_slots;
    return QueueReservation(state_);
}

void RecordQueue::publish(QueueReservation reservation, Record record) noexcept {
    assert(state_ && reservation.state_ == state_);
    assert(record.payload && record.charge && record.descriptor && record.segment_id != 0);
    assert(record.stream_id == record.descriptor->stream_id && record.acceptance_sequence != 0);
    assert(record.charge.logical_bytes() == record.payload.size());
    auto retained_state = std::move(reservation.state_);
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& counters = state_->counters;
        assert(counters.reserved_slots != 0 && counters.queued_records < counters.capacity);
        auto& slot = state_->slots[state_->write_position];
        assert(!slot);
        // All fields have nothrow ownership moves. The queue sees no half Record.
        slot.emplace(std::move(record));
        state_->write_position = (state_->write_position + 1) % counters.capacity;
        --counters.reserved_slots;
        ++counters.queued_records;
        counters.queued_logical_bytes += slot->payload.size();
        counters.high_water_records =
            std::max(counters.high_water_records, counters.queued_records);
        counters.high_water_logical_bytes =
            std::max(counters.high_water_logical_bytes, counters.queued_logical_bytes);
    }
    // The capacity-token reference is released after the queue lock.
}

std::optional<Record> RecordQueue::pop() {
    if (!state_) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto& counters = state_->counters;
    if (counters.queued_records == 0) {
        return std::nullopt;
    }
    auto& slot = state_->slots[state_->read_position];
    assert(slot);
    const auto logical_size = slot->payload.size();
    std::optional<Record> record(std::move(*slot));
    slot.reset();  // Moved-from owners are empty; no backing is released here.
    state_->read_position = (state_->read_position + 1) % counters.capacity;
    --counters.queued_records;
    counters.queued_logical_bytes -= logical_size;
    return record;
}

void RecordQueue::stop_reservations() {
    if (state_) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->counters.accepting = false;
    }
}

RecordQueueSnapshot RecordQueue::snapshot() const {
    if (!state_) {
        return {};
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto result = state_->counters;
    // Only bounded POD reads; no ref destruction or arbitrary observer work.
    // Public queries consume the separately published Pipeline Facts snapshot.
    for (const auto& slot : state_->slots) {
        if (slot && (!result.oldest_receive_monotonic ||
                     slot->received.sample.monotonic_ns < *result.oldest_receive_monotonic)) {
            result.oldest_receive_monotonic = slot->received.sample.monotonic_ns;
        }
    }
    return result;
}

}  // namespace rodaq::pipeline
