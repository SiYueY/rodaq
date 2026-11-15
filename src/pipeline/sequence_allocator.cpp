#include "sequence_allocator.hpp"

namespace rodaq::pipeline {

Result<void> SequenceAllocator::begin_refill(std::uint64_t work_id) {
    if (work_id == 0 || work_id <= last_work_id_) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (refill_work_id_ != 0 || prepared_) {
        return Status(StatusCode::Conflict, {});
    }
    refill_work_id_ = work_id;
    last_work_id_ = work_id;
    return {};
}

Result<void> SequenceAllocator::finish_refill(std::uint64_t work_id, IdentityRange durable_range) {
    if (work_id == 0 || work_id != refill_work_id_) {
        return Status(StatusCode::Conflict, {});
    }
    if (durable_range.begin == 0 || durable_range.begin >= durable_range.end_exclusive ||
        durable_range.begin < reserved_frontier_ || prepared_) {
        refill_work_id_ = 0;
        refill_failure_ = StatusCode::DataLoss;
        return Status(StatusCode::DataLoss, {});
    }
    // A valid gap can result from a previously abandoned reservation; overlap
    // can never be accepted, including a replay of an already consumed range.
    reserved_frontier_ = durable_range.end_exclusive;
    if (active_.begin == active_.end_exclusive) {
        active_ = durable_range;
    } else {
        prepared_ = durable_range;
    }
    refill_work_id_ = 0;
    refill_failure_ = StatusCode::Ok;
    return {};
}

Result<void> SequenceAllocator::finish_failure(std::uint64_t work_id, StatusCode failure) {
    if (work_id == 0 || work_id != refill_work_id_) {
        return Status(StatusCode::Conflict, {});
    }
    if (failure == StatusCode::Ok || static_cast<std::int32_t>(failure) < 0 ||
        static_cast<std::int32_t>(failure) > static_cast<std::int32_t>(StatusCode::Internal)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    refill_work_id_ = 0;
    refill_failure_ = failure;
    return {};
}

Result<std::uint64_t> SequenceAllocator::allocate() {
    if (active_.begin == active_.end_exclusive && prepared_) {
        active_ = *prepared_;
        prepared_.reset();
    }
    if (active_.begin == active_.end_exclusive) {
        return Status(
            refill_failure_ == StatusCode::Ok ? StatusCode::ResourceExhausted : refill_failure_,
            {});
    }
    return active_.begin++;
}

SequenceAllocationSnapshot SequenceAllocator::snapshot() const noexcept {
    return {
        active_.end_exclusive - active_.begin,
        prepared_ ? prepared_->end_exclusive - prepared_->begin : 0, refill_work_id_,
        refill_failure_};
}

}  // namespace rodaq::pipeline
