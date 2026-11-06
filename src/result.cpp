#include "rodaq/result.hpp"

#include <utility>

namespace rodaq {

Result<void>::Result() = default;

Result<void>::Result(Status status)
: result_(nonstd::make_unexpected(
      status.ok() ? Status(StatusCode::UnknownError, "OK status cannot represent a failed result")
                  : std::move(status))) {}

bool Result<void>::ok() const noexcept { return result_.has_value(); }
Result<void>::operator bool() const noexcept { return ok(); }

const Status& Result<void>::status() const& noexcept {
    static const Status success;
    return ok() ? success : result_.error();
}
Status Result<void>::status() && { return ok() ? Status() : std::move(result_.error()); }
void Result<void>::value() const { result_.value(); }

}  // namespace rodaq
