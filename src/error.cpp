#include "rodaq/error.hpp"

#include <utility>

namespace rodaq {

Error::Error(ErrorCode code, std::string message)
: code_(code), native_code_(0), domain_(), message_(std::move(message)) {}

Error::Error(ErrorCode code, std::string domain, std::int64_t native_code, std::string message)
: code_(code),
  native_code_(native_code),
  domain_(std::move(domain)),
  message_(std::move(message)) {}

ErrorCode Error::code() const noexcept { return code_; }

std::int64_t Error::native_code() const noexcept { return native_code_; }

std::string_view Error::domain() const noexcept { return domain_; }

std::string_view Error::message() const noexcept { return message_; }

}  // namespace rodaq
