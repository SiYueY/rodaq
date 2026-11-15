#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <rodaq/result.hpp>

namespace rodaq::persistence {

// Actual byte-content digest, with no semantic domain or JSON conversion.
// A positive size requires readable backing for the duration of the call.
Result<std::string> byte_digest(const void* bytes, std::size_t size);

// Hash exact sealed regular-file bytes using bounded pread backing. The caller
// retains the fd until actual return; file offset is unchanged. Size/type/link
// and before/after identity/change checks reject incomplete or unstable evidence.
Result<std::string> file_digest(int descriptor, std::uint64_t expected_size);

}  // namespace rodaq::persistence
