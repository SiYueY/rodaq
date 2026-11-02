#ifndef RODAQ_RESULT_HPP_
#define RODAQ_RESULT_HPP_

#include "rodaq/error.hpp"

#ifndef nsel_CONFIG_SELECT_EXPECTED
#define nsel_CONFIG_SELECT_EXPECTED nsel_EXPECTED_NONSTD
#endif

#include <nonstd/expected.hpp>

#if expected_lite_MAJOR != 0 || expected_lite_MINOR != 10 || expected_lite_PATCH != 0
#error "RoDAQ V1 requires expected-lite 0.10.0"
#endif

#if nsel_CONFIG_SELECT_EXPECTED != nsel_EXPECTED_NONSTD
#error "RoDAQ V1 requires expected-lite's nonstd implementation"
#endif

static_assert(!nsel_USES_STD_EXPECTED, "RoDAQ V1 must not silently switch Result to std::expected");

namespace rodaq {

/// A recoverable RoDAQ operation result.
///
/// Result<T> is the explicit Public C++ alias frozen by D-025. Only this alias
/// is part of the RoDAQ C++ API contract; expected-lite never enters the Stable
/// C Plugin ABI.
template <typename T>
using Result = nonstd::expected<T, Error>;

}  // namespace rodaq

#endif  // RODAQ_RESULT_HPP_
