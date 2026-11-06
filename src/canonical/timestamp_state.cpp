#include "canonical/timestamp_state.hpp"

namespace rodaq {

Result<void> TimestampState::set(std::size_t slot, std::int64_t reading) {
    if (slot >= kMaxSlots) {
        return Status(StatusCode::InvalidArgument, "timestamp slot is out of range");
    }
    readings_[slot] = reading;
    const auto bit = static_cast<std::uint8_t>(1U << slot);
    present_mask_ = static_cast<std::uint8_t>(present_mask_ | bit);
    return {};
}

bool TimestampState::present(std::size_t slot) const noexcept {
    return slot < kMaxSlots && (present_mask_ & static_cast<std::uint8_t>(1U << slot)) != 0;
}

std::optional<std::int64_t> TimestampState::value(std::size_t slot) const noexcept {
    if (!present(slot)) {
        return std::nullopt;
    }
    return readings_[slot];
}

}  // namespace rodaq
