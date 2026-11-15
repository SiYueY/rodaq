#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <rodaq/result.hpp>

namespace rodaq::uuid {

// Shared private encoding/random primitive; domain wrappers remain distinct.
using Bytes = std::array<std::uint8_t, 16>;

bool nonzero(const Bytes& value) noexcept;
Result<Bytes> parse(std::string_view text);
std::string format(const Bytes& value);
Result<Bytes> generate();

}  // namespace rodaq::uuid
