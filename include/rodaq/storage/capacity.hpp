#pragma once

#include <cstdint>
#include <limits>

#include <rodaq/result.hpp>
#include <rodaq/storage/types.hpp>

namespace rodaq::storage {

/** Conservative checked ceil(bytes*numerator/denominator)+overhead. */
inline Result<std::uint64_t> expansion_bound(
    std::uint64_t bytes, std::uint64_t numerator, std::uint64_t denominator,
    std::uint64_t overhead) {
    if (denominator == 0) {
        return Status(StatusCode::InvalidArgument, "Zero Storage expansion denominator");
    }
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    if (numerator != 0 && bytes > kMax / numerator) {
        return Status(StatusCode::ResourceExhausted, "Storage expansion multiplication overflow");
    }
    const auto product = bytes * numerator;
    auto expanded = product / denominator;
    if (product % denominator != 0) {
        ++expanded;  // A remainder implies the quotient is strictly below UINT64_MAX.
    }
    if (overhead > kMax - expanded) {
        return Status(StatusCode::ResourceExhausted, "Storage expansion addition overflow");
    }
    return expanded + overhead;
}

inline Result<std::uint64_t> record_bound(const CapacityModel& model, std::uint64_t bytes) {
    return expansion_bound(
        bytes, model.payload_expansion_numerator, model.payload_expansion_denominator,
        model.per_record_overhead_bytes);
}

inline Result<std::uint64_t> descriptor_bound(const CapacityModel& model, std::uint64_t bytes) {
    return expansion_bound(
        bytes, model.descriptor_expansion_numerator, model.descriptor_expansion_denominator,
        model.per_descriptor_overhead_bytes);
}

}  // namespace rodaq::storage
