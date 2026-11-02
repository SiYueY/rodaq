#ifndef RODAQ_ERROR_HPP_
#define RODAQ_ERROR_HPP_

#include <cstdint>
#include <string>
#include <string_view>

namespace rodaq {

enum class ErrorCode : std::uint32_t {
    InvalidArgument = 1,
    InvalidState = 2,
    NotFound = 3,
    Conflict = 4,
    ResourceExhausted = 5,
    Unavailable = 6,
    StorageError = 7,
    Timeout = 8,
    InternalError = 9,
};

/// Owns a portable RoDAQ error category and its diagnostic details.
///
/// The strings returned by domain() and message() are borrowed views that
/// remain valid until this object is destroyed, assigned, or moved from.
class Error final {
public:
    /// Creates a portable error with native_code 0 and an empty domain.
    Error(ErrorCode code, std::string message);

    /// Creates an error with explicit backend or middleware diagnostics.
    Error(ErrorCode code, std::string domain, std::int64_t native_code, std::string message);

    ErrorCode code() const noexcept;
    std::int64_t native_code() const noexcept;
    std::string_view domain() const noexcept;
    std::string_view message() const noexcept;

private:
    ErrorCode code_;
    std::int64_t native_code_;
    std::string domain_;
    std::string message_;
};

}  // namespace rodaq

#endif  // RODAQ_ERROR_HPP_
