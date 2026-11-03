#include "runtime/clock_correlation.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <rodaq/error.hpp>

#include <exception>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace rodaq::runtime {

struct ClockCorrelation::State {
    UnixEpochClockAnchor anchor;
};

ClockCorrelation::ClockCorrelation(ClockCorrelation&&) noexcept = default;

ClockCorrelation& ClockCorrelation::operator=(ClockCorrelation&&) noexcept = default;

ClockCorrelation::~ClockCorrelation() = default;

namespace {

Error clock_error(ErrorCode code, std::string message) {
    return Error(code, "clock_correlation", 0, std::move(message));
}

std::int64_t midpoint(std::int64_t first, std::int64_t second) noexcept {
    if (first > second) {
        std::swap(first, second);
    }
    return first / 2 + second / 2 + (first % 2 + second % 2) / 2;
}

}  // namespace

const rodaq_runtime_clock_correlation_v1 ClockCorrelation::kClockCorrelationVtable = {
    RODAQ_RUNTIME_CLOCK_CORRELATION_PROTOCOL_VERSION,
    RODAQ_RUNTIME_CLOCK_CORRELATION_V1_MIN_STRUCT_SIZE,
    &ClockCorrelation::get_unix_epoch_anchor,
};

Result<std::uint64_t> ProjectUnixNanoseconds(
    std::int64_t reading, const UnixEpochClockAnchor& anchor) {
    if (reading >= anchor.clock_reading) {
        const std::uint64_t delta =
            static_cast<std::uint64_t>(reading) - static_cast<std::uint64_t>(anchor.clock_reading);
        if (delta > std::numeric_limits<std::uint64_t>::max() - anchor.unix_anchor_ns) {
            return nonstd::make_unexpected(
                clock_error(ErrorCode::InvalidArgument, "Unix replay timestamp overflows uint64"));
        }
        return anchor.unix_anchor_ns + delta;
    }

    const std::uint64_t magnitude =
        static_cast<std::uint64_t>(anchor.clock_reading) - static_cast<std::uint64_t>(reading);
    if (magnitude > anchor.unix_anchor_ns) {
        return nonstd::make_unexpected(
            clock_error(ErrorCode::InvalidArgument, "Unix replay timestamp is before Unix epoch"));
    }
    return anchor.unix_anchor_ns - magnitude;
}

ClockCorrelation::ClockCorrelation(std::unique_ptr<State> state) noexcept
: state_(std::move(state)) {}

Result<ClockCorrelation> ClockCorrelation::Establish(
    rodaq_clock_handle clock, ClockReadingFunction clock_reading, UnixTimeFunction unix_time) {
    if (clock.value == 0) {
        return nonstd::make_unexpected(clock_error(
            ErrorCode::InvalidArgument, "Clock correlation requires a valid ingress Clock"));
    }
    if (!clock_reading || !unix_time) {
        return nonstd::make_unexpected(
            clock_error(ErrorCode::InvalidArgument, "Clock correlation readers must be provided"));
    }

    try {
        const std::int64_t before = clock_reading();
        const std::uint64_t unix_ns = unix_time();
        const std::int64_t after = clock_reading();
        if (after < before) {
            return nonstd::make_unexpected(clock_error(
                ErrorCode::Unavailable, "Ingress clock moved backward during anchor capture"));
        }
        return FromAnchor(UnixEpochClockAnchor{clock, midpoint(before, after), unix_ns});
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(clock_error(
            ErrorCode::ResourceExhausted, "Memory exhausted while capturing Clock anchor"));
    } catch (const std::exception& error) {
        return nonstd::make_unexpected(clock_error(
            ErrorCode::Unavailable, std::string("Clock anchor capture failed: ") + error.what()));
    } catch (...) {
        return nonstd::make_unexpected(
            clock_error(ErrorCode::Unavailable, "Unknown Clock anchor capture failure"));
    }
}

Result<ClockCorrelation> ClockCorrelation::FromAnchor(UnixEpochClockAnchor anchor) {
    if (anchor.clock.value == 0) {
        return nonstd::make_unexpected(clock_error(
            ErrorCode::InvalidArgument, "Clock correlation requires a valid ingress Clock"));
    }

    try {
        auto state = std::make_unique<State>();
        state->anchor = anchor;
        return ClockCorrelation(std::move(state));
    } catch (const std::bad_alloc&) {
        return nonstd::make_unexpected(clock_error(
            ErrorCode::ResourceExhausted, "Memory exhausted while creating Clock correlation"));
    }
}

Result<UnixEpochClockAnchor> ClockCorrelation::GetAnchor(rodaq_clock_handle clock) const {
    if (clock.value == 0) {
        return nonstd::make_unexpected(
            clock_error(ErrorCode::InvalidArgument, "Clock handle is invalid"));
    }
    if (state_ == nullptr) {
        return nonstd::make_unexpected(
            clock_error(ErrorCode::InvalidState, "Clock correlation object is empty"));
    }
    if (clock.value != state_->anchor.clock.value) {
        return nonstd::make_unexpected(
            clock_error(ErrorCode::NotFound, "No correlation exists for the requested Clock"));
    }
    return state_->anchor;
}

rodaq_interface ClockCorrelation::AbiInterface() noexcept {
    return rodaq_interface{state_.get(), &kClockCorrelationVtable};
}

rodaq_status ClockCorrelation::get_unix_epoch_anchor(
    void* context, rodaq_clock_handle clock, rodaq_unix_epoch_clock_anchor* out_anchor,
    rodaq_error* out_error) noexcept {
    if (out_anchor == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "clock_correlation", 0,
            "Clock anchor query requires a non-null output value", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    const std::uint32_t capacity = out_anchor->struct_size;
    if (capacity < RODAQ_UNIX_EPOCH_CLOCK_ANCHOR_MIN_STRUCT_SIZE) {
        out_anchor->struct_size = 0;
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "clock_correlation", 0,
            "Clock anchor output capacity is below the V1 minimum", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }
    std::memset(out_anchor, 0, std::min<std::size_t>(capacity, sizeof(*out_anchor)));
    if (context == nullptr) {
        assign_rodaq_error(
            RODAQ_STATUS_INVALID_ARGUMENT, "clock_correlation", 0,
            "Clock anchor query context is null", out_error);
        return RODAQ_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto* state = static_cast<const State*>(context);
        if (state->anchor.clock.value != clock.value) {
            assign_rodaq_error(
                RODAQ_STATUS_NOT_FOUND, "clock_correlation", 0,
                "No correlation exists for the requested Clock", out_error);
            return RODAQ_STATUS_NOT_FOUND;
        }
        out_anchor->struct_size = sizeof(rodaq_unix_epoch_clock_anchor);
        out_anchor->reserved = 0;
        if (RODAQ_HAS_FIELD_CAPACITY(rodaq_unix_epoch_clock_anchor, capacity, clock)) {
            out_anchor->clock = state->anchor.clock;
        }
        if (RODAQ_HAS_FIELD_CAPACITY(rodaq_unix_epoch_clock_anchor, capacity, clock_reading)) {
            out_anchor->clock_reading = state->anchor.clock_reading;
        }
        if (RODAQ_HAS_FIELD_CAPACITY(rodaq_unix_epoch_clock_anchor, capacity, unix_anchor_ns)) {
            out_anchor->unix_anchor_ns = state->anchor.unix_anchor_ns;
        }
        return RODAQ_STATUS_OK;
    } catch (const std::exception& error) {
        assign_rodaq_error(
            RODAQ_STATUS_INTERNAL_ERROR, "clock_correlation", 0, error.what(), out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    } catch (...) {
        assign_rodaq_error(
            RODAQ_STATUS_INTERNAL_ERROR, "clock_correlation", 0,
            "Unknown Clock anchor query failure", out_error);
        return RODAQ_STATUS_INTERNAL_ERROR;
    }
}

}  // namespace rodaq::runtime
