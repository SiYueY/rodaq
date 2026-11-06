#ifndef RODAQ_STATUS_HPP_
#define RODAQ_STATUS_HPP_

#include <rodaq/export.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace rodaq {

/// C++ status codes correspond to the C plugin status code values.
enum class StatusCode : std::uint32_t {
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    NotFound = 3,
    Conflict = 4,
    ResourceExhausted = 5,
    Unavailable = 6,
    StorageError = 7,
    Timeout = 8,
    InternalError = 9,
    UnknownError = 10,
};

/// Operation status with optional failure diagnostics.
/// Borrowed string views remain valid until assignment, movement or destruction.
/// OK always has empty diagnostics; unknown codes become UnknownError.
class RODAQ_EXPORT Status final {
public:
    Status() noexcept = default;
    Status(StatusCode status_code, std::string message);
    Status(
        StatusCode status_code, std::string domain, std::int64_t error_code, std::string message);

    bool ok() const noexcept;
    StatusCode status_code() const noexcept;
    std::int64_t error_code() const noexcept;
    std::string_view domain() const noexcept;
    std::string_view message() const noexcept;

private:
    StatusCode status_code_ = StatusCode::Ok;
    std::int64_t error_code_ = 0;
    std::string domain_;
    std::string message_;
};

}  // namespace rodaq

#endif  // RODAQ_STATUS_HPP_
