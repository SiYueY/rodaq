#include "atomic_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <linux/fs.h>
#include <new>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace rodaq::persistence {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int file) noexcept : file_(file) {}

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    ~FileDescriptor() noexcept {
        if (file_ >= 0) {
            close(file_);
        }
    }

    int get() const noexcept { return file_; }

private:
    int file_;
};

bool valid_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

StatusCode error_code(int error) noexcept {
    switch (error) {
        case EEXIST:
        case ENOTEMPTY:
        case EISDIR:
            return StatusCode::Conflict;
        case ENOENT:
            return StatusCode::NotFound;
        case EACCES:
        case EPERM:
        case EROFS:
            return StatusCode::PermissionDenied;
        case ENOSPC:
        case EDQUOT:
        case ENOMEM:
        case EMFILE:
        case ENFILE:
            return StatusCode::ResourceExhausted;
        case EINVAL:
        case ENAMETOOLONG:
        case ELOOP:
        case ENOTDIR:
            return StatusCode::InvalidArgument;
        default:
            return StatusCode::Unavailable;
    }
}

int injected(const FileFaultHook& hook, FileStep step) noexcept {
    if (!hook) {
        return 0;
    }
    try {
        return hook(step);
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    } catch (...) {
        return EIO;
    }
}

Result<std::string> temporary_name() {
    std::array<std::uint8_t, 16> random{};
    std::size_t filled = 0;
    while (filled < random.size()) {
        const auto count = getrandom(random.data() + filled, random.size() - filled, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return Status(StatusCode::Unavailable, {});
        }
        filled += static_cast<std::size_t>(count);
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string name = ".rodaq-tmp-";
    name.reserve(name.size() + random.size() * 2);
    for (const auto byte : random) {
        name += kHex[byte >> 4u];
        name += kHex[byte & 0xfu];
    }
    return name;
}

int synchronize(int file) noexcept {
    int result = 0;
    do {
        result = fsync(file);
    } while (result < 0 && errno == EINTR);
    return result == 0 ? 0 : errno;
}

bool definitely_not_renamed(int error) noexcept {
    switch (error) {
        case EEXIST:
        case ENOTEMPTY:
        case ENOENT:
        case EACCES:
        case EPERM:
        case EROFS:
        case ENOTDIR:
        case EISDIR:
        case ENAMETOOLONG:
        case ENOSYS:
        case EINVAL:
            return true;
        default:
            return false;
    }
}

bool same_snapshot(const struct stat& left, const struct stat& right) noexcept {
    // Replacing the pathname can change this open old inode's ctime/link count
    // without changing any bytes. That is a valid immutable read snapshot.
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
           left.st_size == right.st_size && left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec;
}

}  // namespace

FilePublication publish_file(
    const std::filesystem::path& directory, std::string_view name, std::string_view contents,
    FilePublishMode mode, std::size_t max_bytes, const FileFaultHook& faults) {
    FilePublication outcome;
    if (!valid_name(name) || contents.size() > max_bytes ||
        (mode != FilePublishMode::Create && mode != FilePublishMode::Replace)) {
        outcome.status = Status(StatusCode::InvalidArgument, {});
        return outcome;
    }
    try {
        // All ordinary allocations precede creation of any temporary file.
        const std::string target(name);
        auto generated = temporary_name();
        if (!generated.ok()) {
            outcome.status = generated.status();
            return outcome;
        }
        outcome.retained_temporary = std::move(generated).value();

        auto error = injected(faults, FileStep::OpenDirectory);
        FileDescriptor parent(
            error == 0 ? open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)
                       : -1);
        if (parent.get() < 0) {
            outcome.system_error = error == 0 ? errno : error;
            outcome.status = Status(error_code(outcome.system_error), {});
            outcome.retained_temporary.clear();
            return outcome;
        }

        outcome.step = FileStep::CreateTemporary;
        error = injected(faults, outcome.step);
        FileDescriptor file(
            error == 0 ? openat(
                             parent.get(), outcome.retained_temporary.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)
                       : -1);
        if (file.get() < 0) {
            outcome.system_error = error == 0 ? errno : error;
            outcome.status = Status(error_code(outcome.system_error), {});
            // O_EXCL failure never transfers ownership of an existing pathname.
            outcome.retained_temporary.clear();
            return outcome;
        }

        const auto fail = [&](int failure) {
            outcome.system_error = failure;
            outcome.status = Status(error_code(failure), {});
            if (outcome.disposition == FileDisposition::NotPublished) {
                auto cleanup = injected(faults, FileStep::RemoveTemporary);
                if (cleanup == 0 &&
                    unlinkat(parent.get(), outcome.retained_temporary.c_str(), 0) != 0) {
                    cleanup = errno;
                }
                outcome.cleanup_error = cleanup;
                if (cleanup == 0) {
                    outcome.retained_temporary.clear();
                }
            }
        };

        outcome.step = FileStep::WriteTemporary;
        error = injected(faults, outcome.step);
        std::size_t written = 0;
        while (error == 0 && written < contents.size()) {
            const auto count = write(
                file.get(), contents.data() + written,
                std::min<std::size_t>(contents.size() - written, 1024 * 1024));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                error = count == 0 ? EIO : errno;
            } else {
                written += static_cast<std::size_t>(count);
            }
        }
        if (error != 0) {
            fail(error);
            return outcome;
        }

        outcome.step = FileStep::SyncFile;
        error = injected(faults, outcome.step);
        if (error == 0) {
            error = synchronize(file.get());
        }
        if (error != 0) {
            fail(error);
            return outcome;
        }

        outcome.step = FileStep::Publish;
        error = injected(faults, outcome.step);
        if (error == 0) {
            const auto renamed =
                mode == FilePublishMode::Create
                    ? syscall(
                          SYS_renameat2, parent.get(), outcome.retained_temporary.c_str(),
                          parent.get(), target.c_str(), RENAME_NOREPLACE)
                    : renameat(
                          parent.get(), outcome.retained_temporary.c_str(), parent.get(),
                          target.c_str());
            if (renamed != 0) {
                error = errno;
                if (!definitely_not_renamed(error)) {
                    outcome.disposition = FileDisposition::Uncertain;
                }
            }
        }
        if (error != 0) {
            fail(error);
            return outcome;
        }

        outcome.disposition = FileDisposition::Uncertain;
        outcome.retained_temporary.clear();
        outcome.step = FileStep::SyncDirectory;
        error = injected(faults, outcome.step);
        if (error == 0) {
            error = synchronize(parent.get());
        }
        if (error != 0) {
            fail(error);
            return outcome;
        }

        outcome.disposition = FileDisposition::Durable;
        outcome.step = FileStep::Complete;
        return outcome;
    } catch (const std::bad_alloc&) {
        // No allocation follows temporary creation; hook exceptions are contained.
        outcome.status = Status(StatusCode::ResourceExhausted, {});
        outcome.retained_temporary.clear();
        return outcome;
    }
}

Result<std::string> read_file(
    const std::filesystem::path& directory, std::string_view name, std::size_t max_bytes) {
    if (!valid_name(name)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        const std::string target(name);
        FileDescriptor parent(
            open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (parent.get() < 0) {
            return Status(error_code(errno), {});
        }
        FileDescriptor file(
            openat(parent.get(), target.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (file.get() < 0) {
            return Status(error_code(errno), {});
        }
        struct stat before {};
        if (fstat(file.get(), &before) != 0) {
            return Status(error_code(errno), {});
        }
        if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
            static_cast<std::uint64_t>(before.st_size) > max_bytes) {
            return Status(StatusCode::InvalidArgument, {});
        }
        std::string contents(static_cast<std::size_t>(before.st_size), '\0');
        std::size_t received = 0;
        while (received < contents.size()) {
            const auto count = read(
                file.get(), contents.data() + received,
                std::min<std::size_t>(contents.size() - received, 1024 * 1024));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return Status(count == 0 ? StatusCode::DataLoss : error_code(errno), {});
            }
            received += static_cast<std::size_t>(count);
        }
        char extra = 0;
        ssize_t remaining = 0;
        do {
            remaining = read(file.get(), &extra, 1);
        } while (remaining < 0 && errno == EINTR);
        if (remaining < 0) {
            return Status(error_code(errno), {});
        }
        struct stat after {};
        if (fstat(file.get(), &after) != 0) {
            return Status(error_code(errno), {});
        }
        if (remaining != 0 || !same_snapshot(before, after)) {
            return Status(StatusCode::DataLoss, {});
        }
        return contents;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::persistence
