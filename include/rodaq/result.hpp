#ifndef RODAQ_RESULT_HPP_
#define RODAQ_RESULT_HPP_

#include <rodaq/status.hpp>

#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

#include <tl/expected.hpp>

#if TL_EXPECTED_VERSION_MAJOR != 1 || TL_EXPECTED_VERSION_MINOR != 3 || \
    TL_EXPECTED_VERSION_PATCH != 1
#error "RoDAQ requires tl::expected 1.3.1"
#endif

namespace rodaq {

template <typename T>
class [[nodiscard]] Result final {
    static_assert(std::is_object_v<T>, "Result<T> requires T to be an object type");
    static_assert(!std::is_array_v<T>, "Result<T> does not support array types");
    static_assert(!std::is_const_v<T>, "Result<T> does not support const-qualified types");
    static_assert(!std::is_volatile_v<T>, "Result<T> does not support volatile-qualified types");
    static_assert(!std::is_same_v<T, Status>, "Result<Status> is not supported");

public:
    Result() = delete;

    Result(const T& value) : expected_(value) {}

    Result(T&& value) : expected_(std::move(value)) {}

    Result(const Status& status) : expected_(tl::unexpect, status) {
        if (expected_.error().ok()) {
            std::abort();
        }
    }

    Result(Status&& status) : expected_(tl::unexpect, std::move(status)) {
        if (expected_.error().ok()) {
            std::abort();
        }
    }

    Result(const Result&) = default;
    Result(Result&&) = default;

    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;

    ~Result() = default;

    [[nodiscard]] bool ok() const noexcept { return expected_.has_value(); }

    [[nodiscard]] const Status& status() const& noexcept {
        if (!expected_.has_value()) {
            return expected_.error();
        }

        static const Status ok_status = Status::Ok();
        return ok_status;
    }

    [[nodiscard]] Status status() && noexcept {
        if (!expected_.has_value()) {
            return std::move(expected_).error();
        }

        return Status::Ok();
    }

    T& value() & noexcept {
        if (!expected_.has_value()) {
            std::abort();
        }

        return *expected_;
    }

    const T& value() const& noexcept {
        if (!expected_.has_value()) {
            std::abort();
        }

        return *expected_;
    }

    T&& value() && noexcept {
        if (!expected_.has_value()) {
            std::abort();
        }

        return std::move(*expected_);
    }

    const T&& value() const&& noexcept {
        if (!expected_.has_value()) {
            std::abort();
        }

        return std::move(*expected_);
    }

    T& operator*() & noexcept { return value(); }

    const T& operator*() const& noexcept { return value(); }

    T&& operator*() && noexcept { return std::move(*this).value(); }

    const T&& operator*() const&& noexcept { return std::move(*this).value(); }

    T* operator->() & noexcept { return std::addressof(value()); }

    const T* operator->() const& noexcept { return std::addressof(value()); }

    T* operator->() && = delete;
    const T* operator->() const&& = delete;

private:
    tl::expected<T, Status> expected_;
};

template <>
class [[nodiscard]] Result<void> final {
public:
    Result() = default;

    Result(const Status& status) : expected_(tl::unexpect, status) {
        if (expected_.error().ok()) {
            std::abort();
        }
    }

    Result(Status&& status) : expected_(tl::unexpect, std::move(status)) {
        if (expected_.error().ok()) {
            std::abort();
        }
    }

    Result(const Result&) = default;
    Result(Result&&) = default;

    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;

    ~Result() = default;

    [[nodiscard]] bool ok() const noexcept { return expected_.has_value(); }

    [[nodiscard]] const Status& status() const& noexcept {
        if (!expected_.has_value()) {
            return expected_.error();
        }

        static const Status ok_status = Status::Ok();
        return ok_status;
    }

    [[nodiscard]] Status status() && noexcept {
        if (!expected_.has_value()) {
            return std::move(expected_).error();
        }

        return Status::Ok();
    }

    void value() const noexcept {
        if (!expected_.has_value()) {
            std::abort();
        }
    }

private:
    tl::expected<void, Status> expected_;
};

}  // namespace rodaq

#endif  // RODAQ_RESULT_HPP_
