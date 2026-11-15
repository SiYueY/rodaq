#pragma once

#include <unistd.h>
#include <utility>

namespace rodaq::persistence {

// Private POSIX ownership used by filesystem authorities. Borrow through get();
// move/release is the only ownership transfer. No close retry can hit a reused fd.
class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : value_(other.release()) {}

    FileDescriptor& operator=(FileDescriptor&&) = delete;

    ~FileDescriptor() noexcept {
        if (value_ >= 0) {
            close(value_);
        }
    }

    int get() const noexcept { return value_; }

    int release() noexcept { return std::exchange(value_, -1); }

private:
    int value_;
};

}  // namespace rodaq::persistence
