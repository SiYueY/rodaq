#pragma once

#include <cstddef>
#include <string>

#include <rodaq/result.hpp>

namespace rodaq::persistence {

// Actual byte-content digest, with no semantic domain or JSON conversion.
// A positive size requires readable backing for the duration of the call.
Result<std::string> byte_digest(const void* bytes, std::size_t size);

}  // namespace rodaq::persistence
