#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace rodaq {

enum class StatusCode : std::int32_t {
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    NotFound = 3,
    Conflict = 4,
    ResourceExhausted = 5,
    DeadlineExceeded = 6,
    Unavailable = 7,
    PermissionDenied = 8,
    DataLoss = 9,
    Internal = 10,
};

class [[nodiscard]] Status final {
public:
    Status() noexcept = default;

    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {
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

    [[nodiscard]] std::string_view message() const& noexcept { return message_; }

    std::string_view message() const&& = delete;

private:
    StatusCode code_{StatusCode::Ok};
    std::string message_;
};

}  // namespace rodaq
