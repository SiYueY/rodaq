#include "catalog_state.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

constexpr std::uint32_t kMaxDeleteDepth = 4;
constexpr std::uint32_t kMaxDeleteEntries = 1024;

int rename_noreplace(int dir_fd, const char* from, const char* to) noexcept {
    return static_cast<int>(syscall(SYS_renameat2, dir_fd, from, dir_fd, to, RENAME_NOREPLACE));
}

}  // namespace

Result<void> ArtifactCatalog::delete_artifact(
    ArtifactId id, std::chrono::steady_clock::time_point deadline) {
    if (id.value == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto state = state_;
    std::size_t at = 0;
    std::shared_ptr<CatalogEntry> entry;
    bool already_renamed = false;
    {
        std::lock_guard<std::mutex> lock(state->index);
        at = state->find(id.value);
        if (at == state->slots.size() || !state->slots[at].entry) {
            return Status(StatusCode::NotFound, {});
        }
        auto& slot = state->slots[at];
        if (state->scanning || slot.work != CatalogWork::None || slot.frame) {
            return Status(StatusCode::Conflict, {});
        }
        if (!slot.visible && !slot.deleting_pending) {
            return Status(StatusCode::NotFound, {});
        }
        slot.work = CatalogWork::Delete;
        entry = slot.entry;
        entry->read_blocked = true;
        already_renamed = slot.deleting_pending;
    }

    bool renamed = already_renamed;
    bool cleared = false;
    struct DeleteFlight {
        std::shared_ptr<CatalogState> state;
        std::size_t at;
        std::shared_ptr<CatalogEntry> entry;
        bool& renamed;
        bool& cleared;
        ~DeleteFlight() {
            std::lock_guard<std::mutex> lock(state->index);
            if (!cleared) {
                state->slots[at].work = CatalogWork::None;
                if (!renamed && entry) {
                    // Rename never occurred: artifact stays intact and readable.
                    entry->read_blocked = false;
                }
            }
        }
    } flight{state, at, entry, renamed, cleared};

    try {
        auto volume_ok = state->options.volume->verify();
        if (!volume_ok.ok()) {
            return volume_ok.status();
        }
        if (!already_renamed) {
            auto readers = wait_readers(id, deadline);
            if (!readers.ok()) {
                return readers.status();
            }
        }
        FileDescriptor parent(open(
            state->options.volume->artifacts_directory().c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const std::string final_name = std::to_string(id.value);
        const std::string deleting_name = final_name + ".deleting";

        if (!already_renamed) {
            if ((state->options.delete_faults &&
                 state->options.delete_faults(DeleteStep::RenameToDeleting) != 0) ||
                rename_noreplace(parent.get(), final_name.c_str(), deleting_name.c_str()) != 0) {
                if (errno == ENOENT) {
                    state->corrupt(id.value);
                    return Status(StatusCode::DataLoss, {});
                }
                return Status(StatusCode::Unavailable, {});
            }
            renamed = true;
            {
                std::lock_guard<std::mutex> lock(state->index);
                state->slots[at].visible = false;
                state->slots[at].deleting_pending = true;
            }
        }

        // Physical deletion of <id>.deleting tree.
        if (state->options.delete_faults &&
            state->options.delete_faults(DeleteStep::RemoveTree) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        std::uint32_t remaining = kMaxDeleteEntries;
        auto removed =
            remove_tree_bounded(parent.get(), deleting_name.c_str(), kMaxDeleteDepth, remaining);
        if (!removed.ok()) {
            return removed.status();
        }
        if ((state->options.delete_faults &&
             state->options.delete_faults(DeleteStep::SyncParent) != 0) ||
            fsync(parent.get()) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        volume_ok = state->options.volume->verify();
        if (!volume_ok.ok()) {
            return volume_ok.status();
        }
        if (entry->charge) {
            auto released = entry->charge->release_after_delete();
            if (!released.ok()) {
                return released.status();
            }
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at] = CatalogSlot{};
            cleared = true;
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

}  // namespace rodaq::artifact
