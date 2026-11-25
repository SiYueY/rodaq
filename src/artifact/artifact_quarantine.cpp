#include "recovery_scan.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "../persistence/file_descriptor.hpp"
#include "../task/control_record.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

Result<std::optional<struct stat>> named_entry(int parent, const std::string& name) {
    struct stat value {};
    if (fstatat(parent, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) == 0) {
        return std::optional<struct stat>(value);
    }
    return errno == ENOENT
               ? Result<std::optional<struct stat>>(std::optional<struct stat>{})
               : Result<std::optional<struct stat>>(Status(StatusCode::Unavailable, {}));
}

bool same_entry(const struct stat& before, const struct stat& after) noexcept {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           (before.st_mode & S_IFMT) == (after.st_mode & S_IFMT);
}

Result<void> checkpoint(int fd, const persistence::FileFaultHook& faults) {
    if (faults && faults(persistence::FileStep::SyncDirectory) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    int result = 0;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? Result<void>{} : Result<void>(Status(StatusCode::Unavailable, {}));
}

struct QuarantineName {
    std::string source;
    std::string target;
    std::optional<struct stat> retained;
};

Result<void> quarantine_names(
    const std::shared_ptr<ManagedVolume>& volume, const LedgerRecord& record,
    const std::string& tag, const persistence::FileFaultHook& faults,
    std::optional<std::array<std::optional<struct stat>, 2>>& prior) {
    auto valid = volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    FileDescriptor source(open(
        volume->artifacts_directory().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    FileDescriptor target(open(
        (volume->root() / ".rodaq/quarantine").c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (source.get() < 0 || target.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto segment = std::to_string(record.segment_id);
    // Identity-scoped diagnostic references survive removal of the pre-commit
    // row. These names never authorize normal Catalog registration/adoption.
    const auto prefix = to_string(record.task_id) + ".g" +
                        std::to_string(record.control_generation) + ".r" +
                        std::to_string(record.reset_generation_at_commit) + ".s" + segment + tag;
    std::array<QuarantineName, 2> names{
        {{segment + ".tmp", prefix + ".tmp", {}}, {segment, prefix + ".final", {}}}};
    std::size_t index = 0;
    for (auto& name : names) {
        auto original = named_entry(source.get(), name.source);
        auto destination = named_entry(target.get(), name.target);
        if (!original.ok() || !destination.ok()) {
            return !original.ok() ? original.status() : destination.status();
        }
        if (original.value() && destination.value()) {
            return Status(StatusCode::Conflict, {});
        }
        if (prior) {
            const auto& expected = (*prior)[index];
            if (original.value() || expected.has_value() != destination.value().has_value() ||
                (expected && !same_entry(*expected, *destination.value()))) {
                return Status(StatusCode::DataLoss, {});
            }
        }
        if (original.value()) {
            if (faults && faults(persistence::FileStep::Publish) != 0) {
                return Status(StatusCode::Unavailable, {});
            }
            valid = volume->verify();
            if (!valid.ok()) {
                return valid.status();
            }
            if (syscall(
                    SYS_renameat2, source.get(), name.source.c_str(), target.get(),
                    name.target.c_str(), RENAME_NOREPLACE) != 0) {
                return Status(errno == EEXIST ? StatusCode::Conflict : StatusCode::Unavailable, {});
            }
            name.retained = original.value();
        } else {
            name.retained = destination.value();
        }
        ++index;
    }
    // Destination durability precedes durable source absence across parents.
    valid = checkpoint(target.get(), faults);
    if (!valid.ok()) {
        return valid.status();
    }
    valid = checkpoint(source.get(), faults);
    if (!valid.ok()) {
        return valid.status();
    }
    if (faults && faults(persistence::FileStep::Complete) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    for (const auto& name : names) {
        const auto remaining = named_entry(source.get(), name.source);
        const auto preserved = named_entry(target.get(), name.target);
        if (!remaining.ok() || !preserved.ok()) {
            return !remaining.ok() ? remaining.status() : preserved.status();
        }
        if (remaining.value() || preserved.value().has_value() != name.retained.has_value() ||
            (name.retained && !same_entry(*name.retained, *preserved.value()))) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    valid = volume->verify();
    if (valid.ok()) {
        prior = std::array<std::optional<struct stat>, 2>{names[0].retained, names[1].retained};
    }
    return valid;
}

}  // namespace

Result<void> quarantine_reset_invalidated_artifact(
    std::shared_ptr<ManagedVolume> volume, LedgerStore& ledger, std::uint64_t segment,
    const task::TaskControlRecord& control, persistence::FileFaultHook faults) {
    if (!volume || segment == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::optional<std::array<std::optional<struct stat>, 2>> retained;
    return ledger.abort_committing(segment, [&](const LedgerRecord& record) -> Result<void> {
        if (!(record.task_id == control.task_id) || !(record.volume_id == volume->identity()) ||
            record.control_generation > control.control_generation ||
            record.reset_generation_at_commit >= control.reset_generation ||
            (control.reset_intent &&
             record.control_generation > control.reset_intent->target_control_generation)) {
            return Status(StatusCode::Conflict, {});
        }
        return quarantine_names(volume, record, "", faults, retained);
    });
}

Result<void> quarantine_failed_precommit_artifact(
    std::shared_ptr<ManagedVolume> volume, LedgerStore& ledger, std::uint64_t segment,
    persistence::FileFaultHook faults) {
    if (!volume || segment == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::optional<std::array<std::optional<struct stat>, 2>> retained;
    return ledger.abort_committing(segment, [&](const LedgerRecord& record) -> Result<void> {
        if (!(record.volume_id == volume->identity())) {
            return Status(StatusCode::DataLoss, {});
        }
        return quarantine_names(volume, record, ".precommit", faults, retained);
    });
}

}  // namespace rodaq::artifact
