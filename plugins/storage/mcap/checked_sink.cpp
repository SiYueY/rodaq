#include "checked_sink.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <unistd.h>
#include <utility>

namespace rodaq::mcap_storage {

CheckedSink::CheckedSink(Write write) : write_(std::move(write)) {
    if (!write_) {
        write_ = [](int fd, const void* bytes, std::size_t count) {
            return ::write(fd, bytes, count);
        };
    }
}

CheckedSink::~CheckedSink() noexcept { end(); }

Result<void> CheckedSink::open(int directory, const char* name) {
    if (file_ >= 0 || logical_ != 0 || status_ != StatusCode::Ok) {
        return Status(StatusCode::InvalidState, {});
    }
    file_ = ::openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file_ < 0) {
        fail(errno == EEXIST ? StatusCode::Conflict : StatusCode::Unavailable);
        return Status(status_, {});
    }
    created_ = true;
    return {};
}

void CheckedSink::end() {
    if (file_ >= 0) {
        // Linux close consumes the fd even on EINTR; never retry a reused fd.
        const auto file = std::exchange(file_, -1);
        if (::close(file) != 0) {
            fail(StatusCode::Unavailable);
        }
    }
}

std::uint64_t CheckedSink::size() const { return logical_; }

std::uint64_t CheckedSink::confirmed_bytes() const noexcept { return confirmed_; }

bool CheckedSink::file_created() const noexcept { return created_; }

StatusCode CheckedSink::status() const noexcept { return status_; }

void CheckedSink::fail(StatusCode error) noexcept {
    if (status_ == StatusCode::Ok) {
        status_ = error;
    }
}

void CheckedSink::handleWrite(const std::byte* data, std::uint64_t count) {
    if (count > std::numeric_limits<std::uint64_t>::max() - logical_) {
        logical_ = std::numeric_limits<std::uint64_t>::max();
        fail(StatusCode::ResourceExhausted);
        return;
    }
    logical_ += count;
    if (status_ != StatusCode::Ok || count == 0) {
        return;
    }
    if (file_ < 0 || data == nullptr ||
        count > static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())) {
        fail(StatusCode::InvalidArgument);
        return;
    }
    ssize_t written;
    do {
        written = write_(file_, data, static_cast<std::size_t>(count));
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        fail(errno == ENOSPC ? StatusCode::ResourceExhausted : StatusCode::Unavailable);
        return;
    }
    if (static_cast<std::uint64_t>(written) > count) {
        fail(StatusCode::DataLoss);
        return;
    }
    confirmed_ += static_cast<std::uint64_t>(written);
    if (static_cast<std::uint64_t>(written) != count) {
        fail(StatusCode::DataLoss);
    }
}

}  // namespace rodaq::mcap_storage
