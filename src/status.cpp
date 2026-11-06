#include "rodaq/status.hpp"

#include <utility>

namespace rodaq {
namespace {

StatusCode normalize_status(StatusCode status_code) noexcept {
    switch (status_code) {
        case StatusCode::Ok:
        case StatusCode::InvalidArgument:
        case StatusCode::InvalidState:
        case StatusCode::NotFound:
        case StatusCode::Conflict:
        case StatusCode::ResourceExhausted:
        case StatusCode::Unavailable:
        case StatusCode::StorageError:
        case StatusCode::Timeout:
        case StatusCode::InternalError:
        case StatusCode::UnknownError:
            return status_code;
    }
    return StatusCode::UnknownError;
}

}  // namespace

Status::Status(StatusCode status_code, std::string message)
: Status(status_code, {}, 0, std::move(message)) {}

Status::Status(
    StatusCode status_code, std::string domain, std::int64_t error_code, std::string message)
: status_code_(normalize_status(status_code)) {
    if (!ok()) {
        domain_ = std::move(domain);
        error_code_ = error_code;
        message_ = std::move(message);
    }
}

bool Status::ok() const noexcept { return status_code_ == StatusCode::Ok; }
StatusCode Status::status_code() const noexcept { return status_code_; }
std::string_view Status::domain() const noexcept { return domain_; }
std::int64_t Status::error_code() const noexcept { return error_code_; }
std::string_view Status::message() const noexcept { return message_; }

}  // namespace rodaq
