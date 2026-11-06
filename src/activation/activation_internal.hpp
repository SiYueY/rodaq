#ifndef RODAQ_ACTIVATION_ACTIVATION_INTERNAL_HPP_
#define RODAQ_ACTIVATION_ACTIVATION_INTERNAL_HPP_

#include "activation/activation_state.hpp"

#include <rodaq/plugin/abi_support.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kIngressTimestampRole = "rodaq/acquisition_ingress";
constexpr std::size_t kMaxSecondaryFailures = 8;
constexpr std::size_t kRuntimeClockPluginIndex = std::numeric_limits<std::size_t>::max();

inline Status activation_status(StatusCode status_code, std::string message) {
    return Status(status_code, "runtime", 0, std::move(message));
}

inline std::unique_ptr<LatencyHistogram> make_latency_histogram() {
    auto result = LatencyHistogram::create(1, 60LL * 1'000'000'000LL, 3);
    if (!result.ok()) {
        throw std::runtime_error(std::string(result.status().message()));
    }
    return std::move(result.value());
}

inline LatencyStats latency_snapshot(const LatencyHistogram& histogram) noexcept {
    LatencyStats snapshot;
    snapshot.count = histogram.count();
    if (snapshot.count == 0) {
        return snapshot;
    }
    snapshot.minimum_ns = histogram.min();
    snapshot.maximum_ns = histogram.max();
    snapshot.p50_ns = histogram.p50();
    snapshot.p95_ns = histogram.p95();
    snapshot.p99_ns = histogram.p99();
    snapshot.p999_ns = histogram.percentile(99.9);
    return snapshot;
}

template <typename T, typename HandleType>
Registry<T, HandleType> make_registry(std::size_t capacity, std::uint32_t generation) {
    auto result = Registry<T, HandleType>::create(capacity, generation);
    if (!result.ok()) {
        throw std::runtime_error(std::string(result.status().message()));
    }
    return std::move(result.value());
}

inline std::uint32_t registry_generation(std::uint64_t activation_generation) noexcept {
    const auto generation = static_cast<std::uint32_t>(activation_generation);
    return generation == 0 ? 1U : generation;
}

inline ArenaBytes copy_bytes(
    std::pmr::memory_resource* resource, const std::uint8_t* data, std::size_t size) {
    ArenaBytes result(resource);
    if (size != 0) {
        result.assign(data, data + size);
    }
    return result;
}

inline ArenaString copy_string(std::pmr::memory_resource* resource, std::string_view value) {
    ArenaString result(resource);
    result.assign(value.data(), value.size());
    return result;
}

inline Result<std::int64_t> capture_ingress_reading() {
    const auto count =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
    if (count > std::numeric_limits<std::int64_t>::max()) {
        return activation_status(StatusCode::InternalError, "DAQ ingress clock reading overflowed");
    }
    return static_cast<std::int64_t>(count);
}

inline bool bytes_equal(
    const ArenaBytes& stored, const std::uint8_t* data, std::size_t size) noexcept {
    return stored.size() == size && std::equal(stored.begin(), stored.end(), data);
}

inline bool strings_equal(const ArenaString& stored, std::string_view value) noexcept {
    return stored.size() == value.size() && std::equal(stored.begin(), stored.end(), value.begin());
}

template <typename Struct>
Result<void> write_output_prefix(const Struct& output, Struct* destination) {
    if (destination == nullptr) {
        return activation_status(StatusCode::InvalidArgument, "DAQ output pointer is null");
    }
    const std::uint32_t capacity = destination->struct_size;
    const std::uint32_t initialized = initialized_prefix_size<Struct>(capacity);
    std::memset(destination, 0, initialized);
    std::memcpy(destination, &output, initialized);
    destination->struct_size = initialized;
    return {};
}

inline Status status_from_abi_diagnostic(const rodaq_status& error) {
    const std::string_view domain(
        error.domain, std::min<std::size_t>(error.domain_size, sizeof(error.domain)));
    const std::string_view message(
        error.message, std::min<std::size_t>(error.message_size, sizeof(error.message)));
    StatusCode status_code = StatusCode::InternalError;
    switch (error.status_code) {
        case RODAQ_STATUS_INVALID_ARGUMENT:
            status_code = StatusCode::InvalidArgument;
            break;
        case RODAQ_STATUS_INVALID_STATE:
            status_code = StatusCode::InvalidState;
            break;
        case RODAQ_STATUS_NOT_FOUND:
            status_code = StatusCode::NotFound;
            break;
        case RODAQ_STATUS_CONFLICT:
            status_code = StatusCode::Conflict;
            break;
        case RODAQ_STATUS_RESOURCE_EXHAUSTED:
            status_code = StatusCode::ResourceExhausted;
            break;
        case RODAQ_STATUS_UNAVAILABLE:
            status_code = StatusCode::Unavailable;
            break;
        case RODAQ_STATUS_STORAGE_ERROR:
            status_code = StatusCode::StorageError;
            break;
        case RODAQ_STATUS_TIMEOUT:
            status_code = StatusCode::Timeout;
            break;
        case RODAQ_STATUS_UNKNOWN_ERROR:
            status_code = StatusCode::UnknownError;
            break;
        default:
            status_code = StatusCode::InternalError;
            break;
    }
    return Status(status_code, std::string(domain), error.error_code, std::string(message));
}

inline rodaq_acquisition_submit_result submit_result(
    rodaq_admission_outcome outcome, rodaq_admission_rejection_reason reason,
    rodaq_payload_disposition disposition) noexcept {
    return rodaq_acquisition_submit_result{
        sizeof(rodaq_acquisition_submit_result), outcome, reason, disposition, {0, 0},
    };
}

}  // namespace rodaq

#endif  // RODAQ_ACTIVATION_ACTIVATION_INTERNAL_HPP_
