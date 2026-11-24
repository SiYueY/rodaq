#include "catalog_state.hpp"
#include "recovery_scan.hpp"

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

#include "../persistence/digest.hpp"

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

Result<CatalogEntry> read_deleting_tree(const CatalogOptions& options, const LedgerRecord& record) {
    CatalogEntry entry;
    entry.info.summary.artifact_id = {record.segment_id};
    entry.info.summary.segment_id = record.segment_id;
    entry.info.summary.task_id = record.task_id;
    entry.read_blocked = true;
    const auto path = options.volume->artifacts_directory() / std::to_string(record.segment_id);
    FileDescriptor directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) {
        return errno == ENOENT ? Result<CatalogEntry>(std::move(entry))
                               : Result<CatalogEntry>(Status(StatusCode::DataLoss, {}));
    }
    if (fstat(directory.get(), &entry.directory) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    FileDescriptor manifest_fd(
        openat(directory.get(), "manifest.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (manifest_fd.get() < 0) {
        // After Manifest removal, only an empty final directory is deletable.
        // remove_files verifies that exact empty set before any further effect.
        return errno == ENOENT ? Result<CatalogEntry>(std::move(entry))
                               : Result<CatalogEntry>(Status(StatusCode::DataLoss, {}));
    }
    if (fstat(manifest_fd.get(), &entry.manifest) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    auto bytes = persistence::read_file(path, "manifest.json", options.manifest.max_bytes);
    if (!bytes.ok()) {
        return bytes.status();
    }
    auto expected = persistence::byte_digest(bytes.value().data(), bytes.value().size());
    auto actual = persistence::file_digest(manifest_fd.get(), bytes.value().size());
    if (!expected.ok() || !actual.ok()) {
        return !expected.ok() ? expected.status() : actual.status();
    }
    if (expected.value() != actual.value()) {
        return Status(StatusCode::DataLoss, {});
    }
    auto manifest = decode_manifest(bytes.value(), options.manifest);
    if (!manifest.ok()) {
        return manifest.status();
    }
    auto binding = validate_manifest_ledger_binding(manifest.value(), record, options.manifest);
    if (!binding.ok()) {
        return binding.status();
    }

    entry.files.reserve(manifest.value().files.size());
    for (const auto& file : manifest.value().files) {
        CatalogFile observed;
        observed.path = file.relative_path;
        FileDescriptor fd(openat(
            directory.get(), file.relative_path.c_str(),
            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (fd.get() < 0) {
            if (errno != ENOENT) {
                return Status(StatusCode::DataLoss, {});
            }
        } else {
            if (fstat(fd.get(), &observed.identity) != 0) {
                return Status(StatusCode::Unavailable, {});
            }
            auto digest = persistence::file_digest(fd.get(), file.physical_size);
            if (!digest.ok()) {
                return digest.status();
            }
            if (digest.value() != file.digest) {
                return Status(StatusCode::DataLoss, {});
            }
        }
        entry.files.push_back(std::move(observed));
    }
    return entry;
}

}  // namespace

Result<void> recover_deleting_artifact(
    std::shared_ptr<ManagedVolume> volume, LedgerStore& ledger, std::uint64_t segment,
    const ManifestLimits& limits, std::function<int(DeleteStep)> faults) {
    if (!volume || segment == 0 || !validate_manifest_limits(limits).ok()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto valid = volume->verify();
        if (!valid.ok()) {
            return valid.status();
        }
        auto record = ledger.get(segment);
        if (!record.ok() && record.status().code() == StatusCode::Unavailable) {
            auto reconciled = ledger.reconcile(segment);
            if (!reconciled.ok()) {
                return reconciled.status();
            }
            record = ledger.get(segment);
        }
        if (!record.ok()) {
            return record.status();
        }
        if (!(record.value().volume_id == volume->identity()) ||
            (record.value().state != LedgerState::Deleting &&
             record.value().state != LedgerState::Deleted)) {
            return Status(StatusCode::InvalidState, {});
        }
        CatalogOptions options;
        options.volume = std::move(volume);
        options.manifest = limits;
        options.delete_faults = std::move(faults);
        if (record.value().state == LedgerState::Deleted) {
            return prove_artifact_absence(options, segment);
        }
        auto entry = read_deleting_tree(options, record.value());
        if (!entry.ok()) {
            return entry.status();
        }
        auto removed = remove_files(options, entry.value(), false);
        if (!removed.ok()) {
            return removed.status();
        }
        auto absent = prove_artifact_absence(options, segment);
        if (!absent.ok()) {
            return absent.status();
        }
        record.value().state = LedgerState::Deleted;
        record.value().deleted_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        return ledger.advance(record.value(), LedgerState::Deleting);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

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
