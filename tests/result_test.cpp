#include <rodaq/result.hpp>
#include <rodaq/status.hpp>

#include <cassert>
#include <csignal>
#include <memory>
#include <type_traits>
#include <utility>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using rodaq::Result;
using rodaq::Status;
using rodaq::StatusCode;

struct Point {
    int value;
};

template <class T, class = void>
struct HasLvalueArrow : std::false_type {};

template <class T>
struct HasLvalueArrow<T, std::void_t<decltype(std::declval<T&>().operator->())>> : std::true_type {
};

template <class T, class = void>
struct HasRvalueArrow : std::false_type {};

template <class T>
struct HasRvalueArrow<T, std::void_t<decltype(std::declval<T&&>().operator->())>> : std::true_type {
};

template <class T, class = void>
struct CanMessageRvalue : std::false_type {};

template <class T>
struct CanMessageRvalue<T, std::void_t<decltype(std::declval<T&&>().message())>> : std::true_type {
};

static_assert(!std::is_default_constructible_v<Result<int>>);
static_assert(std::is_default_constructible_v<Result<void>>);
static_assert(!std::is_copy_constructible_v<Result<std::unique_ptr<int>>>);
static_assert(std::is_move_constructible_v<Result<std::unique_ptr<int>>>);
static_assert(std::is_same_v<decltype(std::declval<Result<int>&>().value()), int&>);
static_assert(std::is_same_v<decltype(std::declval<const Result<int>&>().value()), const int&>);
static_assert(std::is_same_v<decltype(std::declval<Result<int>&&>().value()), int&&>);
static_assert(std::is_same_v<decltype(std::declval<const Result<int>&&>().value()), const int&&>);
static_assert(HasLvalueArrow<Result<Point>>::value);
static_assert(!HasRvalueArrow<Result<Point>>::value);
static_assert(!CanMessageRvalue<Status>::value);

void verify_value_result() {
    Result<int> value(7);
    assert(value.ok());
    assert(value.value() == 7);
    assert(*value == 7);
    assert(value.status().ok());

    Result<int> copy = value;
    assert(copy.ok());
    assert(copy.value() == 7);

    Result<int> moved = std::move(value);
    assert(moved.ok());
    assert(moved.value() == 7);

    Result<Point> point(Point{9});
    assert(point.ok());
    assert(point->value == 9);
}

void verify_error_result() {
    const Status timeout(StatusCode::DeadlineExceeded, "timed out");
    Result<int> failed(timeout);
    assert(!failed.ok());
    assert(failed.status().code() == StatusCode::DeadlineExceeded);
    assert(failed.status().message() == "timed out");

    Result<int> moved_error(Status(StatusCode::Unavailable, "offline"));
    assert(!moved_error.ok());
    assert(moved_error.status().code() == StatusCode::Unavailable);
    assert(moved_error.status().message() == "offline");

    Result<int> rvalue_error(Status(StatusCode::Internal, "internal"));
    auto status = std::move(rvalue_error).status();
    assert(!status.ok());
    assert(status.code() == StatusCode::Internal);
    assert(status.message() == "internal");
}

void verify_void_result() {
    Result<void> done;
    assert(done.ok());
    done.value();
    assert(done.status().ok());

    Result<void> failed(Status(StatusCode::Conflict, "conflict"));
    assert(!failed.ok());
    assert(failed.status().code() == StatusCode::Conflict);
    assert(failed.status().message() == "conflict");

    Result<void> moved(Status(StatusCode::Unavailable, "disk"));
    auto status = std::move(moved).status();
    assert(!status.ok());
    assert(status.code() == StatusCode::Unavailable);
    assert(status.message() == "disk");
}

void verify_move_only_result() {
    Result<std::unique_ptr<int>> value(std::make_unique<int>(42));
    assert(value.ok());
    assert(**value == 42);

    auto moved = std::move(value);
    assert(moved.ok());
    assert(**moved == 42);
}

void verify_status() {
    Status default_status;
    assert(default_status.ok());
    assert(default_status.code() == StatusCode::Ok);
    assert(default_status.message().empty());

    assert(Status::Ok().ok());

    Status error(StatusCode::InvalidArgument, "bad argument");
    assert(!error.ok());
    assert(error.code() == StatusCode::InvalidArgument);
    assert(error.message() == "bad argument");

    const Status& error_ref = error;
    assert(error_ref.message() == "bad argument");

    Status copy = error;
    assert(copy.message() == "bad argument");

    Status moved = std::move(copy);
    assert(moved.message() == "bad argument");
}

#ifndef _WIN32
template <class F>
void expect_abort(F action) {
    const pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        action();
        _exit(0);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGABRT);
}

void verify_contract_violations() {
    expect_abort([] {
        Result<int> failed(Status(StatusCode::DeadlineExceeded, "timeout"));
        (void)failed.value();
    });

    expect_abort([] {
        Result<void> failed(Status(StatusCode::DeadlineExceeded, "timeout"));
        failed.value();
    });

    expect_abort([] {
        Result<int> invalid(Status::Ok());
        (void)invalid;
    });

    expect_abort([] {
        Result<void> invalid(Status::Ok());
        (void)invalid;
    });
}
#endif
}  // namespace

int main() {
    verify_value_result();
    verify_error_result();
    verify_void_result();
    verify_move_only_result();
    verify_status();
#ifndef _WIN32
    verify_contract_violations();
#endif
}
