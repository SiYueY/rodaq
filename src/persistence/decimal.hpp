#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::persistence {

inline Result<std::uint64_t> unsigned_decimal(std::string_view value) {
    if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == '0') ||
        value.front() < '0' || value.front() > '9') {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return number;
}

inline Result<std::int64_t> signed_decimal(std::string_view value) {
    if (value.empty() || value.size() > 20 || value == "-0") {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto digits = value.front() == '-' ? value.substr(1) : value;
    if (digits.empty() || (digits.size() > 1 && digits.front() == '0') || digits.front() < '0' ||
        digits.front() > '9') {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::int64_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    return number;
}

}  // namespace rodaq::persistence
