#include "catalog_state.hpp"

#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "../uuid.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

bool audit_expired(WallTime deleted, WallTime now, std::chrono::nanoseconds window) noexcept {
    if (now < deleted) {
        return false;
    }
    // Unsigned subtraction represents the full nonnegative signed-time span
    // without overflowing chrono's signed nanoseconds at opposite extremes.
    const auto elapsed = static_cast<std::uint64_t>(now.time_since_epoch().count()) -
                         static_cast<std::uint64_t>(deleted.time_since_epoch().count());
    return elapsed >= static_cast<std::uint64_t>(window.count());
}

Result<void> empty_quarantine(const std::filesystem::path& path) {
    FileDescriptor directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto close_directory = [](DIR* value) noexcept { (void)closedir(value); };
    std::unique_ptr<DIR, decltype(close_directory)> entries(
        fdopendir(directory.get()), close_directory);
    if (!entries) {
        return Status(StatusCode::Unavailable, {});
    }
    (void)directory.release();
    errno = 0;
    while (const auto* item = readdir(entries.get())) {
        const std::string_view name(item->d_name);
        if (name != "." && name != "..") {
            // Until Recovery has classified references, unknown quarantine
            // contents cannot be used as proof that this identity is unreferenced.
            return Status(StatusCode::Conflict, {});
        }
        errno = 0;
    }
    if (errno != 0 || fsync(dirfd(entries.get())) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    return {};
}

Result<void> absent_names(int parent, std::uint64_t segment) {
    const auto name = std::to_string(segment);
    for (const auto& candidate : {name, name + ".tmp"}) {
        struct stat attributes {};
        if (fstatat(parent, candidate.c_str(), &attributes, AT_SYMLINK_NOFOLLOW) == 0) {
            return Status(StatusCode::DataLoss, {});
        }
        if (errno != ENOENT) {
            return Status(StatusCode::Unavailable, {});
        }
    }
    return {};
}

Result<void> prove_absence(const CatalogOptions& options, std::uint64_t segment) {
    auto verified = options.volume->verify();
    if (!verified.ok()) {
        return verified.status();
    }
    const auto& path = options.volume->artifacts_directory();
    FileDescriptor parent(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    auto absent = absent_names(parent.get(), segment);
    if (!absent.ok()) {
        return absent.status();
    }
    verified = empty_quarantine(path.parent_path() / "quarantine");
    if (!verified.ok()) {
        return verified.status();
    }
    if ((options.delete_faults && options.delete_faults(DeleteStep::SyncParent) != 0) ||
        fsync(parent.get()) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    absent = absent_names(parent.get(), segment);
    if (!absent.ok()) {
        return absent.status();
    }
    verified = empty_quarantine(path.parent_path() / "quarantine");
    return verified.ok() ? options.volume->verify() : verified;
}

}  // namespace

Result<bool> ArtifactCatalog::collect_deleted(
    TaskId task, std::uint64_t committed_recording_floor_ns, WallTime now,
    std::chrono::nanoseconds audit_window) {
    if (!uuid::nonzero(task.value) || audit_window <= std::chrono::nanoseconds::zero()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto state = state_;
    std::size_t at = state->slots.size();
    std::shared_ptr<CatalogEntry> entry;
    std::uint64_t segment = 0;
    {
        std::lock_guard<std::mutex> lock(state->index);
        if (state->rebuilding) {
            return Status(StatusCode::Conflict, {});
        }
        if (state->options.volume->protection() != StatusCode::Ok ||
            state->options.ledger->protection() != StatusCode::Ok) {
            return Status(
                state->options.volume->protection() != StatusCode::Ok
                    ? state->options.volume->protection()
                    : state->options.ledger->protection(),
                {});
        }
        for (std::size_t i = 0; i < state->slots.size(); ++i) {
            auto& slot = state->slots[i];
            if (!slot.entry || !(slot.entry->info.summary.task_id == task) ||
                slot.entry->info.summary.recording_range.end_ns > committed_recording_floor_ns ||
                slot.work != CatalogWork::None || slot.frame || slot.entry->readers != 0 ||
                !slot.entry->read_blocked || slot.failure != StatusCode::Ok) {
                continue;
            }
            if (!slot.collection_pending) {
                auto record = state->options.ledger->get(slot.segment_id);
                if (!record.ok()) {
                    return record.status();
                }
                if (record.value().state != LedgerState::Deleted || !record.value().deleted_at ||
                    !audit_expired(*record.value().deleted_at, now, audit_window)) {
                    continue;
                }
                if (!(record.value().task_id == task) ||
                    !(record.value().volume_id == state->options.volume->identity())) {
                    return Status(StatusCode::DataLoss, {});
                }
            }
            slot.work = CatalogWork::Collect;
            at = i;
            segment = slot.segment_id;
            entry = slot.entry;
            break;
        }
    }
    if (!entry) {
        return false;
    }
    struct CollectionFlight {
        std::shared_ptr<CatalogState> state;
        std::size_t at;
        std::uint64_t segment;
        ~CollectionFlight() {
            std::lock_guard<std::mutex> lock(state->index);
            if (state->slots[at].segment_id == segment) {
                state->slots[at].work = CatalogWork::None;
            }
        }
    } flight{state, at, segment};
    try {
        auto absent = prove_absence(state->options, segment);
        if (!absent.ok()) {
            if (absent.status().code() == StatusCode::DataLoss) {
                state->corrupt(segment);
                state->options.capacity->protect(StatusCode::DataLoss);
            }
            return absent.status();
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].collection_pending = true;
        }
        auto collected = state->options.ledger->collect_deleted(segment);
        if (!collected.ok() && collected.status().code() != StatusCode::NotFound) {
            if (collected.status().code() == StatusCode::DataLoss) {
                state->corrupt(segment);
                state->options.capacity->protect(StatusCode::DataLoss);
            }
            return collected.status();
        }
        // If unlink settled but a subsequent proof fails, collection_pending
        // retains the actual charge until the same owner can prove absence again.
        absent = prove_absence(state->options, segment);
        if (!absent.ok()) {
            if (absent.status().code() == StatusCode::DataLoss) {
                state->corrupt(segment);
                state->options.capacity->protect(StatusCode::DataLoss);
            }
            return absent.status();
        }
        auto released = entry->charge ? entry->charge->release_after_gc()
                                      : Result<void>(Status(StatusCode::DataLoss, {}));
        if (!released.ok()) {
            state->corrupt(segment);
            state->options.capacity->protect(StatusCode::DataLoss);
            return released.status();
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at] = CatalogSlot{};
        }
        return true;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

}  // namespace rodaq::artifact
