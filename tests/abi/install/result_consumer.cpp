#include <nonstd/expected.hpp>
#include <rodaq/error.hpp>
#include <rodaq/result.hpp>

rodaq::Result<int> make_value() { return 7; }

rodaq::Result<int> make_error() {
    return nonstd::make_unexpected(
        rodaq::Error(rodaq::ErrorCode::Unavailable, "installed consumer"));
}
