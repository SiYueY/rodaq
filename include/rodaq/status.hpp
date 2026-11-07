#ifndef RODAQ_STATUS_HPP_
#define RODAQ_STATUS_HPP_

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq {

/// C++ status codes correspond to the C plugin status code values.
enum class StatusCode : std::int32_t {
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
/// Constructing a non-default Status with Ok is a contract violation.
class [[nodiscard]] Status final {
public:
    Status() noexcept = default;

    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {
        if (code_ == StatusCode::Ok) {
            std::abort();
        }
    }

    Status(StatusCode code, std::string domain, std::int64_t error_code, std::string message)
    : code_(code),
      domain_(std::move(domain)),
      error_code_(error_code),
      message_(std::move(message)) {
        if (code_ == StatusCode::Ok) {
            std::abort();
        }
    }

    [[nodiscard]] static Status Ok() noexcept { return {}; }

    Status(const Status&) = default;
    Status(Status&&) noexcept = default;

    Status& operator=(const Status&) = default;
    Status& operator=(Status&&) noexcept = default;

    ~Status() = default;

    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }

    [[nodiscard]] StatusCode code() const noexcept { return code_; }

    [[nodiscard]] std::string_view domain() const noexcept { return domain_; }

    [[nodiscard]] std::int64_t error_code() const noexcept { return error_code_; }

    [[nodiscard]] std::string_view message() const& noexcept { return message_; }

    std::string_view message() const&& = delete;

private:
    StatusCode code_{StatusCode::Ok};
    std::string domain_;
    std::int64_t error_code_ = 0;
    std::string message_;
};

}  // namespace rodaq

#endif  // RODAQ_STATUS_HPP_
