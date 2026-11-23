#include "catalog_state.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

const char* reason_text(DeleteReason reason) noexcept {
    switch (reason) {
        case DeleteReason::Retention:
            return "retention";
        case DeleteReason::Reset:
            return "reset";
        case DeleteReason::ManualRecovery:
            return "manual-recovery";
    }
    return nullptr;
}

bool same_directory_inode(const struct stat& expected, const struct stat& actual) noexcept {
    return S_ISDIR(actual.st_mode) && expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino;
}

Result<void> verify_remaining(int directory, const CatalogEntry& entry) {
    FileDescriptor scan(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (scan.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto close_directory = [](DIR* value) noexcept { (void)closedir(value); };
    std::unique_ptr<DIR, decltype(close_directory)> entries(fdopendir(scan.get()), close_directory);
    if (!entries) {
        return Status(StatusCode::Unavailable, {});
    }
    (void)scan.release();
    std::size_t count = 0;
    errno = 0;
    while (const auto* item = readdir(entries.get())) {
        const std::string_view name(item->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (++count > entry.files.size() + 1) {
            return Status(StatusCode::DataLoss, {});
        }
        const auto found = std::find_if(
            entry.files.begin(), entry.files.end(),
            [&](const auto& file) { return file.path == name; });
        const struct stat* expected = name == "manifest.json"      ? &entry.manifest
                                      : found != entry.files.end() ? &found->identity
                                                                   : nullptr;
        struct stat actual {};
        if (!expected || fstatat(directory, item->d_name, &actual, AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_regular_file(*expected, actual)) {
            return Status(StatusCode::DataLoss, {});
        }
        errno = 0;
    }
    return errno == 0 ? Result<void>{} : Result<void>(Status(StatusCode::Unavailable, {}));
}

Result<void> unlink_known(
    int directory, const char* name, const struct stat& expected, const CatalogOptions& options,
    DeleteStep step) {
    struct stat actual {};
    if (fstatat(directory, name, &actual, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? Result<void>{} : Result<void>(Status(StatusCode::Unavailable, {}));
    }
    if (!same_regular_file(expected, actual)) {
        return Status(StatusCode::DataLoss, {});
    }
    if ((options.delete_faults && options.delete_faults(step) != 0) ||
        unlinkat(directory, name, 0) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    return {};
}

Result<void> sync_directory(int directory, const CatalogOptions& options, DeleteStep step) {
    if ((options.delete_faults && options.delete_faults(step) != 0) || fsync(directory) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    return {};
}

Result<void> remove_files(
    const CatalogOptions& options, const CatalogEntry& entry, bool already_deleted) {
    auto volume = options.volume->verify();
    if (!volume.ok()) {
        return volume.status();
    }
    FileDescriptor parent(open(
        options.volume->artifacts_directory().c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto name = std::to_string(entry.info.summary.segment_id);
    FileDescriptor directory(
        openat(parent.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0 && errno != ENOENT) {
        return Status(StatusCode::DataLoss, {});
    }
    if (directory.get() >= 0) {
        struct stat actual {};
        if (already_deleted || fstat(directory.get(), &actual) != 0 ||
            !same_directory_inode(entry.directory, actual)) {
            return Status(StatusCode::DataLoss, {});
        }
        auto verified = verify_remaining(directory.get(), entry);
        if (!verified.ok()) {
            return verified.status();
        }
        volume = options.volume->verify();
        if (!volume.ok()) {
            return volume.status();
        }
        for (const auto& file : entry.files) {
            auto removed = unlink_known(
                directory.get(), file.path.c_str(), file.identity, options, DeleteStep::UnlinkFile);
            if (!removed.ok()) {
                return removed.status();
            }
        }
        auto synced = sync_directory(directory.get(), options, DeleteStep::SyncDirectory);
        if (!synced.ok()) {
            return synced.status();
        }
        auto removed = unlink_known(
            directory.get(), "manifest.json", entry.manifest, options, DeleteStep::UnlinkManifest);
        if (!removed.ok()) {
            return removed.status();
        }
        synced = sync_directory(directory.get(), options, DeleteStep::SyncDirectory);
        if (!synced.ok()) {
            return synced.status();
        }
        if (fstatat(parent.get(), name.c_str(), &actual, AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_directory_inode(entry.directory, actual)) {
            return Status(StatusCode::DataLoss, {});
        }
        if ((options.delete_faults && options.delete_faults(DeleteStep::RemoveDirectory) != 0) ||
            unlinkat(parent.get(), name.c_str(), AT_REMOVEDIR) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
    }
    auto synced = sync_directory(parent.get(), options, DeleteStep::SyncParent);
    if (!synced.ok()) {
        return synced.status();
    }
    return options.volume->verify();
}

}  // namespace

Result<void> ArtifactCatalog::delete_artifact(
    ArtifactId id, std::uint64_t generation, DeleteReason reason, WallTime now,
    std::chrono::steady_clock::time_point deadline) {
    const auto* spelling = reason_text(reason);
    if (id.value == 0 || generation == 0 || !spelling) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto state = state_;
    std::size_t at = 0;
    std::shared_ptr<CatalogEntry> entry;
    {
        std::lock_guard<std::mutex> lock(state->index);
        at = state->find(id.value);
        if (at == state->slots.size() || !state->slots[at].entry) {
            return Status(StatusCode::NotFound, {});
        }
        auto& slot = state->slots[at];
        if (state->rebuilding || slot.work != CatalogWork::None || slot.frame ||
            slot.collection_pending) {
            return Status(StatusCode::Conflict, {});
        }
        slot.work = CatalogWork::Delete;
        entry = slot.entry;
        entry->read_blocked = true;
    }
    struct DeleteFlight {
        std::shared_ptr<CatalogState> state;
        std::size_t at;
        ~DeleteFlight() {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].work = CatalogWork::None;
        }
    } flight{state, at};
    try {
        auto readers = wait_readers(id, deadline);
        if (!readers.ok()) {
            return readers.status();
        }
        auto record = state->options.ledger->get(id.value);
        if (!record.ok() && record.status().code() == StatusCode::Unavailable) {
            auto reconciled = state->options.ledger->reconcile(id.value);
            if (!reconciled.ok()) {
                return reconciled.status();
            }
            record = state->options.ledger->get(id.value);
        }
        if (!record.ok()) {
            return record.status();
        }
        if (!(record.value().volume_id == state->options.volume->identity()) ||
            !(record.value().task_id == entry->info.summary.task_id)) {
            return Status(StatusCode::DataLoss, {});
        }
        if (record.value().state == LedgerState::Committed) {
            // Missing/corrupt historical data is not a legal-delete crash window.
            // Prove the immutable committed set before accepting its first intent.
            auto verified = verify_committed(state->options, record.value());
            if (!verified.ok()) {
                state->corrupt(id.value);
                if (verified.status().code() == StatusCode::DataLoss) {
                    state->options.capacity->protect(StatusCode::DataLoss);
                }
                return verified.status();
            }
            if (!same_segment_directory(entry->directory, verified.value()->directory) ||
                !same_regular_file(entry->manifest, verified.value()->manifest)) {
                state->corrupt(id.value);
                state->options.capacity->protect(StatusCode::DataLoss);
                return Status(StatusCode::DataLoss, {});
            }
            record.value().state = LedgerState::Deleting;
            record.value().delete_generation = generation;
            record.value().delete_reason = spelling;
            auto accepted = state->options.ledger->advance(record.value(), LedgerState::Committed);
            if (!accepted.ok()) {
                return accepted.status();
            }
        } else if (
            record.value().state != LedgerState::Deleting &&
            record.value().state != LedgerState::Deleted) {
            return Status(StatusCode::InvalidState, {});
        }
        if (record.value().delete_generation != generation ||
            record.value().delete_reason != spelling) {
            return Status(StatusCode::Conflict, {});
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].visible = false;
        }
        const bool deleted = record.value().state == LedgerState::Deleted;
        auto removed = remove_files(state->options, *entry, deleted);
        if (!removed.ok()) {
            if (removed.status().code() == StatusCode::DataLoss) {
                state->corrupt(id.value);
                state->options.capacity->protect(StatusCode::DataLoss);
            }
            return removed.status();
        }
        if (!deleted) {
            record.value().state = LedgerState::Deleted;
            record.value().deleted_at = now;
            auto completed = state->options.ledger->advance(record.value(), LedgerState::Deleting);
            if (!completed.ok()) {
                return completed.status();
            }
        }
        return entry->charge ? entry->charge->release_after_delete()
                             : Result<void>(Status(StatusCode::DataLoss, {}));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

}  // namespace rodaq::artifact
