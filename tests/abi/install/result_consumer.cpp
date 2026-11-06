#include <rodaq/status.hpp>
#include <rodaq/result.hpp>

rodaq::Result<int> make_value() { return 7; }

rodaq::Result<int> make_failure() {
    return rodaq::Status(rodaq::StatusCode::Unavailable, "installed consumer");
}

rodaq::Result<void> make_success() { return {}; }

rodaq::Result<void> make_void_failure() {
    return rodaq::Status(rodaq::StatusCode::Unavailable, "installed void consumer");
}
