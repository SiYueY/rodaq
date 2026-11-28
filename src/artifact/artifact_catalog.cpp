#include "artifact_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "catalog_state.hpp"
#include "startup_scan.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

Result<void> cleanup_staging(const CatalogOptions& options, std::uint64_t segment_id) {
    try {
        auto verified = options.volume->verify();
        if (!verified.ok()) {
            return verified.status();
        }
        FileDescriptor parent(open(
            options.volume->artifacts_directory().c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto final_name = std::to_string(segment_id);
        struct stat info {};
        // Publication may have succeeded before a later fsync/verify failure.
        // Never release a reservation while a final directory still exists.
        if (fstatat(parent.get(), final_name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
            return Status(StatusCode::DataLoss, {});
        }
        if (errno != ENOENT) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto staging_name = final_name + ".tmp";
        std::uint32_t remaining = 1024;
        auto removed = remove_tree_bounded(parent.get(), staging_name.c_str(), 4, remaining);
        if (!removed.ok()) {
            return removed.status();
        }
        if (options.durability == DurabilityProfile::PowerLossHardened &&
            fsync(parent.get()) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        return options.volume->verify();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

}  // namespace

std::size_t CatalogState::find(std::uint64_t id) const noexcept {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].segment_id == id) {
            return i;
        }
    }
    return slots.size();
}

void CatalogState::corrupt(std::uint64_t id) noexcept {
    std::lock_guard<std::mutex> lock(index);
    const auto at = find(id);
    if (at != slots.size()) {
        slots[at].visible = false;
        slots[at].failure = StatusCode::DataLoss;
        if (slots[at].entry) {
            slots[at].entry->read_blocked = true;
        }
    }
}

ArtifactCatalog::ArtifactCatalog(std::shared_ptr<CatalogState> state) noexcept
: state_(std::move(state)) {}

Result<std::shared_ptr<ArtifactCatalog>> ArtifactCatalog::create(CatalogOptions options) {
    if (!options.volume || !options.capacity || !validate_manifest_limits(options.manifest).ok() ||
        options.max_artifacts == 0 || options.max_artifacts > 65536 ||
        options.max_read_leases == 0 || options.max_read_leases > 4096 ||
        options.max_read_bytes == 0 || options.max_read_bytes > 16 * 1024 * 1024 ||
        options.read_lease_duration <= std::chrono::nanoseconds::zero() ||
        options.read_lease_duration > std::chrono::hours{24}) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto verified = options.volume->verify();
    if (!verified.ok()) {
        return verified.status();
    }
    try {
        auto state = std::make_shared<CatalogState>();
        state->slots.resize(options.max_artifacts);
        state->readers.resize(options.max_read_leases);
        state->cancel_scratch.resize(options.max_read_leases);
        state->options = std::move(options);
        return std::shared_ptr<ArtifactCatalog>(new ArtifactCatalog(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<StartupScanReport> ArtifactCatalog::startup_scan() {
    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->index);
        if (state->scanning || std::any_of(
                                   state->slots.begin(), state->slots.end(),
                                   [](const auto& slot) { return slot.segment_id != 0; })) {
            return Status(StatusCode::InvalidState, {});
        }
        state->scanning = true;
    }
    struct ScanningGuard {
        std::shared_ptr<CatalogState> state;
        ~ScanningGuard() {
            std::lock_guard<std::mutex> lock(state->index);
            state->scanning = false;
        }
    } guard{state};

    auto report = scan_volume(state->options);
    if (!report.ok()) {
        std::lock_guard<std::mutex> lock(state->index);
        state->startup_health = report.status().code();
        state->options.capacity->protect(report.status().code());
        return report.status();
    }
    if (report.value().entries.size() > state->slots.size()) {
        std::lock_guard<std::mutex> lock(state->index);
        state->startup_health = StatusCode::ResourceExhausted;
        state->options.capacity->protect(StatusCode::ResourceExhausted);
        return Status(StatusCode::ResourceExhausted, {});
    }
    try {
        std::size_t at = 0;
        for (const auto& entry : report.value().entries) {
            auto charge =
                state->options.capacity->adopt_startup_artifact(entry->info.summary.physical_bytes);
            if (!charge.ok()) {
                std::lock_guard<std::mutex> lock(state->index);
                state->startup_health = charge.status().code();
                state->options.capacity->protect(charge.status().code());
                return charge.status();
            }
            entry->charge.emplace(std::move(charge.value()));
            std::lock_guard<std::mutex> lock(state->index);
            auto& slot = state->slots[at++];
            slot.segment_id = entry->info.summary.segment_id;
            slot.entry = entry;
            slot.visible = true;
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->startup_corrupt_count = report.value().corrupt_count;
            state->startup_health = report.value().health;
        }
        return report;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> ArtifactCatalog::abort(SealedSegment& sealed) {
    if (sealed.segment_id == 0 || sealed.work_id == 0 ||
        sealed.manifest.identity.segment_id != sealed.segment_id ||
        !(sealed.manifest.identity.volume_id == state_->options.volume->identity()) ||
        !(sealed.manifest.identity.task_id == sealed.task_id)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    {
        std::lock_guard<std::mutex> lock(state_->index);
        if (state_->scanning || state_->find(sealed.segment_id) != state_->slots.size()) {
            sealed.reservation.volume.invalidate_usage();
            return Status(StatusCode::Conflict, {});
        }
    }
    auto cleaned = cleanup_staging(state_->options, sealed.segment_id);
    if (!cleaned.ok()) {
        sealed.reservation.volume.invalidate_usage();
        return cleaned.status();
    }
    return sealed.reservation.volume.settle_aborted_after_cleanup();
}

Result<void> ArtifactCatalog::commit(SealedSegment sealed, bool* settled) {
    if (settled) {
        *settled = false;
    }
    auto state = state_;
    if (sealed.segment_id == 0 || sealed.work_id == 0 ||
        sealed.manifest.identity.segment_id != sealed.segment_id ||
        !(sealed.manifest.identity.volume_id == state->options.volume->identity()) ||
        !(sealed.manifest.identity.task_id == sealed.task_id)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto id = sealed.segment_id;
    std::size_t at = state->slots.size();
    std::shared_ptr<CommitFrame> frame;
    bool claimed = false;

    const auto fail_before_publish = [&](StatusCode failure) -> Result<void> {
        auto cleaned = cleanup_staging(state->options, id);
        if (cleaned.ok() && frame) {
            auto aborted = frame->committer
                               ? frame->committer->abort_after_cleanup()
                               : frame->sealed.reservation.volume.settle_aborted_after_cleanup();
            if (aborted.ok() && settled) {
                *settled = true;
            }
            if (!aborted.ok()) {
                cleaned = aborted.status();
            }
        }
        if (!cleaned.ok()) {
            state->options.capacity->protect(cleaned.status().code());
        }
        if (frame) {
            frame->committer.reset();
        }
        if (frame) {
            frame->files.reset();
        }
        std::lock_guard<std::mutex> lock(state->index);
        auto& slot = state->slots[at];
        slot = CatalogSlot{};
        return cleaned.ok() ? Result<void>(Status(failure, {})) : Result<void>(cleaned.status());
    };

    // Before a Catalog claim, the incoming job still owns the handoff.
    // On rejection clean only if no other Catalog owner claims that identity.
    const auto fail_unclaimed = [&](StatusCode failure) -> Result<void> {
        auto aborted = abort(frame ? frame->sealed : sealed);
        if (aborted.ok() && settled) {
            *settled = true;
        }
        return aborted.ok() ? Result<void>(Status(failure, {})) : Result<void>(aborted.status());
    };

    try {
        const auto work_id = sealed.work_id;
        frame = std::make_shared<CommitFrame>(std::move(sealed));
        {
            std::unique_lock<std::mutex> lock(state->index);
            if (state->scanning) {
                lock.unlock();
                return fail_unclaimed(StatusCode::Conflict);
            }
            if (state->options.volume->protection() != StatusCode::Ok) {
                const auto failure = state->options.volume->protection();
                lock.unlock();
                return fail_unclaimed(failure);
            }
            at = state->find(id);
            if (at != state->slots.size()) {
                // A move-only handoff cannot legitimately be resubmitted.
                // Do not delete or adopt another owner's staging/final files.
                frame->sealed.reservation.volume.invalidate_usage();
                return Status(StatusCode::Conflict, {});
            }
            const auto empty = std::find_if(
                state->slots.begin(), state->slots.end(),
                [](const auto& slot) { return slot.segment_id == 0; });
            if (empty == state->slots.end()) {
                lock.unlock();
                return fail_unclaimed(StatusCode::ResourceExhausted);
            }
            at = static_cast<std::size_t>(empty - state->slots.begin());
            empty->segment_id = id;
            empty->work_id = work_id;
            empty->frame = frame;
            empty->work = CatalogWork::Commit;
            claimed = true;
        }
        auto& input = frame->sealed;
        auto files = SealedFiles::open(
            state->options.volume, id, input.files, input.envelope,
            input.manifest_limits.max_bytes);
        if (!files.ok()) {
            return fail_before_publish(files.status().code());
        }
        frame->files = std::move(files.value());
        if (frame->files->storage_bytes() != input.reservation.storage_bytes) {
            return fail_before_publish(StatusCode::DataLoss);
        }
        const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        input.manifest.identity.committed_at = now;
        input.manifest.files = frame->files->evidence();
        auto committer = SegmentCommitter::create(
            frame->files, input.manifest, input.manifest_limits, input.reservation,
            state->options.durability);
        if (!committer.ok()) {
            return fail_before_publish(committer.status().code());
        }
        frame->committer = std::move(committer.value());
        auto committed = frame->committer->commit();
        if (!committed.ok()) {
            return fail_before_publish(committed.status().code());
        }
        // Final directory is now published. Verify and expose in Catalog.
        auto entry = verify_final_artifact(state->options, id);
        if (!entry.ok()) {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].work = CatalogWork::None;
            state->slots[at].failure = entry.status().code();
            state->slots[at].frame.reset();
            return entry.status();
        }
        auto charge = frame->committer->take_committed_charge();
        if (!charge.ok()) {
            state->corrupt(id);
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].work = CatalogWork::None;
            state->slots[at].failure = charge.status().code();
            state->slots[at].frame.reset();
            return charge.status();
        }
        entry.value()->charge.emplace(std::move(charge.value()));
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].entry = std::move(entry.value());
            state->slots[at].visible = true;
            state->slots[at].work = CatalogWork::None;
            state->slots[at].failure = StatusCode::Ok;
            state->slots[at].frame.reset();
        }
        // Release pinned Commit file descriptors outside the index lock.
        frame->committer.reset();
        if (settled) {
            *settled = true;
        }
        return {};
    } catch (const std::bad_alloc&) {
        return claimed ? fail_before_publish(StatusCode::ResourceExhausted)
                       : fail_unclaimed(StatusCode::ResourceExhausted);
    } catch (...) {
        return claimed ? fail_before_publish(StatusCode::Internal)
                       : fail_unclaimed(StatusCode::Internal);
    }
}

Result<bool> ArtifactCatalog::retire_eligible(TaskId task) const {
    auto state = state_;
    if (state->options.volume->protection() != StatusCode::Ok) {
        return false;
    }
    std::lock_guard<std::mutex> lock(state->index);
    if (state->scanning || state->startup_health != StatusCode::Ok) {
        return false;
    }
    for (const auto& slot : state->slots) {
        if (slot.segment_id == 0) {
            continue;
        }
        const bool belongs = (slot.entry && slot.entry->info.summary.task_id == task) ||
                             (slot.frame && slot.frame->sealed.task_id == task);
        if (!belongs) {
            if (!slot.entry && !slot.frame) {
                return false;
            }
            continue;
        }
        if (slot.visible || slot.deleting_pending || slot.work != CatalogWork::None || slot.frame ||
            slot.failure != StatusCode::Ok || (slot.entry && slot.entry->readers != 0)) {
            return false;
        }
    }
    return true;
}

Result<std::vector<ArtifactId>> ArtifactCatalog::reset_targets(TaskId task) const {
    auto state = state_;
    if (state->options.volume->protection() != StatusCode::Ok) {
        return Status(state->options.volume->protection(), {});
    }
    try {
        std::vector<ArtifactId> targets;
        {
            std::lock_guard<std::mutex> lock(state->index);
            if (state->scanning) {
                return Status(StatusCode::Conflict, {});
            }
            for (const auto& slot : state->slots) {
                if (slot.segment_id == 0 || !slot.entry ||
                    !(slot.entry->info.summary.task_id == task)) {
                    continue;
                }
                if (slot.visible || slot.deleting_pending) {
                    targets.push_back(ArtifactId{slot.segment_id});
                }
            }
        }
        std::sort(targets.begin(), targets.end(), [](ArtifactId a, ArtifactId b) {
            return a.value < b.value;
        });
        return targets;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

CatalogSnapshot ArtifactCatalog::snapshot() const {
    auto state = state_;
    CatalogSnapshot result;
    std::lock_guard<std::mutex> lock(state->index);
    result.protection = state->options.volume->protection();
    if (result.protection == StatusCode::Ok && state->startup_health != StatusCode::Ok) {
        result.protection = state->startup_health;
    }
    result.corrupt_artifacts = state->startup_corrupt_count;
    for (const auto& slot : state->slots) {
        if (slot.segment_id == 0) {
            continue;
        }
        if (slot.visible && slot.entry) {
            ++result.visible;
        }
        if (slot.failure == StatusCode::DataLoss && !slot.visible) {
            ++result.corrupt_artifacts;
        }
        result.commit_inflight += slot.work == CatalogWork::Commit ? 1 : 0;
        result.delete_inflight += slot.work == CatalogWork::Delete ? 1 : 0;
    }
    result.read_leases = state->read_leases;
    result.reads_inflight = state->active_reads.load();
    return result;
}

}  // namespace rodaq::artifact
