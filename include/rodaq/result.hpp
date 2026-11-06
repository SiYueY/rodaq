#ifndef RODAQ_RESULT_HPP_
#define RODAQ_RESULT_HPP_

#include "rodaq/status.hpp"

#include <type_traits>
#include <utility>

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

/// A value or a non-OK Status; the failure alternative is never mutable.
template <typename T>
class Result final {
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Status>, "Result<Status> is ambiguous");

public:
    Result() : Result(Status(StatusCode::UnknownError, {})) {}

    template <
        typename U = T,
        typename = std::enable_if_t<
            std::is_constructible_v<T, U&&> && !std::is_same_v<std::decay_t<U>, Result> &&
            !std::is_same_v<std::decay_t<U>, Status> &&
            !std::is_same_v<std::decay_t<U>, Result<void>>>>
    Result(U&& value) : result_(std::forward<U>(value)) {}

    Result(Status status)
    : result_(nonstd::make_unexpected(normalize_failure(std::move(status)))) {}

    bool ok() const noexcept { return result_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Status& status() const& noexcept {
        static const Status success;
        return ok() ? success : result_.error();
    }
    Status status() && { return ok() ? Status() : std::move(result_.error()); }
    /// Requires ok(); the returned value follows this object's lifetime.
    T& value() & { return result_.value(); }
    const T& value() const& { return result_.value(); }
    T&& value() && { return std::move(result_).value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    static Status normalize_failure(Status status) {
        return status.ok()
                   ? Status(StatusCode::UnknownError, "OK status cannot represent a failed result")
                   : std::move(status);
    }
    nonstd::expected<T, Status> result_;
};

/// A successful no-value operation or a non-OK diagnostic.
template <>
class Result<void> final {
public:
    Result();
    Result(Status status);

    bool ok() const noexcept;
    explicit operator bool() const noexcept;
    const Status& status() const& noexcept;
    Status status() &&;
    /// Requires ok().
    void value() const;

private:
    nonstd::expected<void, Status> result_;
};

}  // namespace rodaq

#endif  // RODAQ_RESULT_HPP_
