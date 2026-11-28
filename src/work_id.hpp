#pragma once

#include <cstdint>

#include <rodaq/result.hpp>

namespace rodaq {

// One process-wide Core work identity space. Zero is invalid; exhaustion never
// wraps into a previously used identity. Persisted domain identities are separate.
Result<std::uint64_t> next_work_id();

}  // namespace rodaq
