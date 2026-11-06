#ifndef RODAQ_CANONICAL_TIMESTAMP_STATE_HPP_
#define RODAQ_CANONICAL_TIMESTAMP_STATE_HPP_

#include <rodaq/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rodaq {

class TimestampState final {
public:
    static constexpr std::size_t kMaxSlots = 4;

    Result<void> set(std::size_t slot, std::int64_t reading);
    bool present(std::size_t slot) const noexcept;
    std::optional<std::int64_t> value(std::size_t slot) const noexcept;
    std::uint8_t present_mask() const noexcept { return present_mask_; }
    std::size_t slot_count() const noexcept { return kMaxSlots; }

private:
    std::uint8_t present_mask_ = 0;
    std::array<std::int64_t, kMaxSlots> readings_{};
};

}  // namespace rodaq

#endif  // RODAQ_CANONICAL_TIMESTAMP_STATE_HPP_
