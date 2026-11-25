#include "artifact_catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "catalog_state.hpp"
#include "../pipeline/recording_segment.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

constexpr std::uint32_t kMaxPurgeDepth = 4;
constexpr std::uint32_t kMaxPurgeEntries = 1024;

Result<void> remove_tree_bounded(
    int parent_fd, const char* name, std::uint32_t depth, std::uint32_t& remaining) {
    if (remaining == 0) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    --remaining;
    struct stat info {};
    if (fstatat(parent_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno == ENOENT ? Result<void>{} : Result<void>(Status(StatusCode::Unavailable, {}));
    }
    if (S_ISREG(info.st_mode)) {
        if (info.st_nlink != 1) {
            return Status(StatusCode::DataLoss, {});
        }
        return unlinkat(parent_fd, name, 0) == 0
                   ? Result<void>{}
                   : Result<void>(Status(StatusCode::Unavailable, {}));
    }
    if (!S_ISDIR(info.st_mode) || depth == 0) {
        return Status(StatusCode::DataLoss, {});
    }
    FileDescriptor opened(openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (opened.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto close_dir = [](DIR* value) noexcept { (void)closedir(value); };
    std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(opened.get()), close_dir);
    if (!entries) {
        return Status(StatusCode::Unavailable, {});
    }
    (void)opened.release();
    const int child_fd = dirfd(entries.get());
    std::vector<std::string> children;
    errno = 0;
    while (const auto* item = readdir(entries.get())) {
        const std::string_view child(item->d_name);
        if (child == "." || child == "..") {
            continue;
        }
        children.emplace_back(child);
        if (children.size() > remaining) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        errno = 0;
    }
    if (errno != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    for (const auto& child : children) {
        auto removed = remove_tree_bounded(child_fd, child.c_str(), depth - 1, remaining);
        if (!removed.ok()) {
            return removed.status();
        }
    }
    if (fsync(child_fd) != 0) {
        return Status(StatusCode::Unavailable, {});
    }
    entries.reset();
    if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        return Status(StatusCode::Unavailable, {});
    }
    return {};
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

Result<bool> CatalogState::public_entry(const CatalogSlot& slot) const {
    if (!slot.visible || !slot.entry) {
        return false;
    }
    auto authority = options.ledger->get(slot.segment_id);
    if (!authority.ok()) {
        return Status(
            authority.status().code() == StatusCode::NotFound ? StatusCode::DataLoss
                                                              : authority.status().code(),
            {});
    }
    return authority.value().state == LedgerState::Committed;
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
    if (!options.volume || !options.ledger || !options.capacity ||
        !validate_manifest_limits(options.manifest).ok() ||
        options.max_artifacts < options.ledger->capacity() || options.max_artifacts > 65536 ||
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
        state->options = std::move(options);
        return std::shared_ptr<ArtifactCatalog>(new ArtifactCatalog(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> ArtifactCatalog::commit(const pipeline::SealedPipelineSegment& sealed) {
    auto state = state_;
    if (!sealed.segment || sealed.work_id == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto id = sealed.segment->metadata.segment_id();
    std::size_t at = state->slots.size();
    std::shared_ptr<CommitFrame> frame;
    bool claimed = false;
    const auto finish = [&](StatusCode failure) -> Result<void> {
        std::lock_guard<std::mutex> lock(state->index);
        state->slots[at].work = CatalogWork::None;
        state->slots[at].failure = failure;
        return failure == StatusCode::Ok ? Result<void>{} : Result<void>(Status(failure, {}));
    };
    try {
        frame = std::make_shared<CommitFrame>(sealed);
        {
            std::lock_guard<std::mutex> lock(state->index);
            if (state->rebuilding) {
                return Status(StatusCode::Conflict, {});
            }
            at = state->find(id);
            if (at != state->slots.size()) {
                const auto& existing = state->slots[at];
                if (existing.work_id != sealed.work_id || existing.work != CatalogWork::None ||
                    existing.collection_pending) {
                    return Status(StatusCode::Conflict, {});
                }
                if (state->options.volume->protection() != StatusCode::Ok) {
                    return Status(state->options.volume->protection(), {});
                }
                auto published = state->public_entry(existing);
                if (!published.ok()) {
                    return published.status();
                }
                return published.value() ? Result<void>{}
                       : existing.failure != StatusCode::Ok
                           ? Result<void>(Status(existing.failure, {}))
                           : Result<void>(Status(StatusCode::InvalidState, {}));
            }
            const auto empty = std::find_if(
                state->slots.begin(), state->slots.end(),
                [](const auto& slot) { return slot.segment_id == 0; });
            if (empty == state->slots.end()) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            at = static_cast<std::size_t>(empty - state->slots.begin());
            empty->segment_id = id;
            empty->work_id = sealed.work_id;
            empty->frame = frame;
            empty->work = CatalogWork::Commit;
            claimed = true;
        }
        const auto& input = frame->sealed;
        auto files = SealedFiles::open(
            state->options.volume, id, input.files, input.envelope,
            input.manifest_limits.max_bytes);
        if (!files.ok()) {
            return finish(files.status().code());
        }
        frame->files = std::move(files.value());
        auto manifest = input.segment->metadata.capture_manifest(frame->files->evidence());
        if (!manifest.ok()) {
            return finish(manifest.status().code());
        }
        auto reservation = input.segment->capacity.handoff_sealed(frame->files->storage_bytes());
        if (!reservation.ok()) {
            return finish(reservation.status().code());
        }
        frame->reservation.emplace(std::move(reservation.value()));
        auto committer = SegmentCommitter::create(
            frame->files, manifest.value(), input.manifest_limits, *frame->reservation);
        if (!committer.ok()) {
            return finish(committer.status().code());
        }
        frame->committer = std::move(committer.value());
        auto committed = frame->committer->commit(*state->options.ledger);
        if (!committed.ok()) {
            return finish(committed.status().code());
        }
        auto entry = verify_committed(state->options, frame->committer->ledger_record());
        if (!entry.ok()) {
            return finish(entry.status().code());
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].entry = entry.value();
            state->slots[at].visible = true;
        }
        auto charge = frame->committer->take_committed_charge();
        if (!charge.ok()) {
            state->corrupt(id);
            return finish(charge.status().code());
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            entry.value()->charge.emplace(std::move(charge.value()));
            state->slots[at].frame.reset();
        }
        // Drop pinned Commit file descriptors outside the index lock. Retained
        // disk/metadata charge now belongs to the Catalog entry, not its cache age.
        frame->committer.reset();
        return finish(StatusCode::Ok);
    } catch (const std::bad_alloc&) {
        return claimed ? finish(StatusCode::ResourceExhausted)
                       : Result<void>(Status(StatusCode::ResourceExhausted, {}));
    } catch (...) {
        return claimed ? finish(StatusCode::Internal)
                       : Result<void>(Status(StatusCode::Internal, {}));
    }
}

Result<void> ArtifactCatalog::rebuild() {
    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->index);
        if (state->rebuilding || std::any_of(
                                     state->slots.begin(), state->slots.end(),
                                     [](const auto& slot) { return slot.segment_id != 0; })) {
            return Status(StatusCode::InvalidState, {});
        }
        state->rebuilding = true;
    }
    struct Rebuilding {
        std::shared_ptr<CatalogState> state;
        ~Rebuilding() {
            std::lock_guard<std::mutex> lock(state->index);
            state->rebuilding = false;
        }
    } rebuilding{state};
    try {
        auto records = state->options.ledger->recovery_snapshot();
        if (!records.ok()) {
            return records.status();
        }
        StatusCode failure = StatusCode::Ok;
        std::size_t at = 0;
        for (const auto& record : records.value()) {
            if (record.state == LedgerState::Deleted) {
                if (!(record.volume_id == state->options.volume->identity())) {
                    return Status(StatusCode::DataLoss, {});
                }
                auto absent = prove_artifact_absence(state->options, record.segment_id);
                if (!absent.ok()) {
                    return absent.status();
                }
                auto entry = std::make_shared<CatalogEntry>();
                entry->info.summary.artifact_id = {record.segment_id};
                entry->info.summary.segment_id = record.segment_id;
                entry->info.summary.task_id = record.task_id;
                entry->info.summary.pipeline_id = record.pipeline_id;
                entry->read_blocked = true;
                auto charge = state->options.capacity->claim_recovered_artifact(0);
                if (!charge.ok()) {
                    return charge.status();
                }
                auto deleted = charge.value().release_after_delete();
                if (!deleted.ok()) {
                    return deleted.status();
                }
                entry->charge.emplace(std::move(charge.value()));
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.entry = std::move(entry);
                continue;
            }
            if (record.state != LedgerState::Committed) {
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.failure = StatusCode::InvalidState;
                if (failure == StatusCode::Ok) {
                    failure = StatusCode::InvalidState;
                }
                continue;
            }
            auto verified = verify_committed(state->options, record);
            if (!verified.ok()) {
                failure = verified.status().code();
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.failure = failure;
                continue;
            }
            auto charge = state->options.capacity->claim_recovered_artifact(
                verified.value()->info.summary.physical_bytes);
            if (!charge.ok()) {
                return charge.status();
            }
            verified.value()->charge.emplace(std::move(charge.value()));
            std::lock_guard<std::mutex> lock(state->index);
            auto& slot = state->slots[at++];
            slot.segment_id = record.segment_id;
            slot.entry = std::move(verified.value());
            slot.visible = true;
        }
        return failure == StatusCode::Ok ? Result<void>{} : Result<void>(Status(failure, {}));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> ArtifactCatalog::rebuild(const VolumeRecoveryReport& report) {
    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->index);
        if (state->rebuilding || !state->quarantined.empty() ||
            std::any_of(state->slots.begin(), state->slots.end(), [](const auto& slot) {
                return slot.segment_id != 0;
            })) {
            return Status(StatusCode::InvalidState, {});
        }
        state->rebuilding = true;
    }
    struct Rebuilding {
        std::shared_ptr<CatalogState> state;
        ~Rebuilding() {
            std::lock_guard<std::mutex> lock(state->index);
            state->rebuilding = false;
        }
    } rebuilding{state};
    try {
        auto records = state->options.ledger->recovery_snapshot();
        if (!records.ok()) {
            return records.status();
        }
        if (records.value().size() > state->slots.size()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        std::size_t at = 0;
        for (const auto& record : records.value()) {
            if (record.state == LedgerState::Deleted) {
                if (!(record.volume_id == state->options.volume->identity())) {
                    return Status(StatusCode::DataLoss, {});
                }
                auto entry = std::make_shared<CatalogEntry>();
                entry->info.summary.artifact_id = {record.segment_id};
                entry->info.summary.segment_id = record.segment_id;
                entry->info.summary.task_id = record.task_id;
                entry->info.summary.pipeline_id = record.pipeline_id;
                entry->read_blocked = true;
                auto charge = state->options.capacity->claim_recovered_artifact(0);
                if (!charge.ok()) {
                    return charge.status();
                }
                auto deleted = charge.value().release_after_delete();
                if (!deleted.ok()) {
                    return deleted.status();
                }
                entry->charge.emplace(std::move(charge.value()));
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.entry = std::move(entry);
                continue;
            }
            if (record.state != LedgerState::Committed) {
                return Status(StatusCode::InvalidState, {});
            }
            const bool known_corrupt =
                std::find(
                    report.corrupted_committed_segments.begin(),
                    report.corrupted_committed_segments.end(),
                    record.segment_id) != report.corrupted_committed_segments.end();
            if (known_corrupt) {
                auto entry = std::make_shared<CatalogEntry>();
                entry->info.summary.artifact_id = {record.segment_id};
                entry->info.summary.segment_id = record.segment_id;
                entry->info.summary.task_id = record.task_id;
                entry->info.summary.pipeline_id = record.pipeline_id;
                entry->read_blocked = true;
                auto charge = state->options.capacity->claim_recovered_artifact(0);
                if (!charge.ok()) {
                    return charge.status();
                }
                entry->charge.emplace(std::move(charge.value()));
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.entry = std::move(entry);
                slot.failure = StatusCode::DataLoss;
                continue;
            }
            auto verified = verify_committed(state->options, record);
            if (!verified.ok()) {
                return verified.status();
            }
            auto charge = state->options.capacity->claim_recovered_artifact(
                verified.value()->info.summary.physical_bytes);
            if (!charge.ok()) {
                return charge.status();
            }
            verified.value()->charge.emplace(std::move(charge.value()));
            std::lock_guard<std::mutex> lock(state->index);
            auto& slot = state->slots[at++];
            slot.segment_id = record.segment_id;
            slot.entry = std::move(verified.value());
            slot.visible = true;
        }

        std::vector<std::shared_ptr<RetainedQuarantine>> retained;
        retained.reserve(report.quarantined.size());
        for (const auto& item : report.quarantined) {
            auto charge = state->options.capacity->claim_recovered_artifact(item.physical_bytes);
            if (!charge.ok()) {
                return charge.status();
            }
            auto entry = std::make_shared<RetainedQuarantine>();
            entry->info = item;
            entry->charge.emplace(std::move(charge.value()));
            retained.push_back(std::move(entry));
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->quarantined = std::move(retained);
            state->recovery_health = report.health;
        }
        if (!report.usage_complete || report.health != StatusCode::Ok) {
            state->options.capacity->protect(
                report.health != StatusCode::Ok ? report.health : StatusCode::DataLoss);
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<bool> ArtifactCatalog::retire_eligible(TaskId task) const {
    auto state = state_;
    if (state->options.volume->protection() != StatusCode::Ok ||
        state->options.ledger->protection() != StatusCode::Ok) {
        return false;
    }
    auto records = state->options.ledger->recovery_snapshot();
    if (!records.ok()) {
        return false;
    }
    for (const auto& record : records.value()) {
        if (record.task_id == task && record.state != LedgerState::Deleted) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(state->index);
    if (state->rebuilding) {
        return false;
    }
    for (const auto& slot : state->slots) {
        if (slot.segment_id == 0) {
            continue;
        }
        const bool belongs = (slot.entry && slot.entry->info.summary.task_id == task) ||
                             (slot.frame && slot.frame->sealed.segment &&
                              slot.frame->sealed.segment->metadata.task_id() == task);
        if (!belongs) {
            if (!slot.entry && !slot.frame) {
                return false;
            }
            continue;
        }
        if (slot.visible || slot.work != CatalogWork::None || slot.frame ||
            slot.failure != StatusCode::Ok || (slot.entry && slot.entry->readers != 0)) {
            return false;
        }
    }
    for (const auto& item : state->quarantined) {
        if (!item || !item->info.task_id || *item->info.task_id == task) {
            return false;
        }
    }
    return true;
}

Result<bool> ArtifactCatalog::has_task_references(TaskId task) const {
    auto state = state_;
    if (state->options.volume->protection() != StatusCode::Ok ||
        state->options.ledger->protection() != StatusCode::Ok) {
        return true;
    }
    auto records = state->options.ledger->recovery_snapshot();
    if (!records.ok()) {
        return true;
    }
    for (const auto& record : records.value()) {
        if (record.task_id == task) {
            return true;
        }
    }
    std::lock_guard<std::mutex> lock(state->index);
    if (state->rebuilding) {
        return true;
    }
    for (const auto& slot : state->slots) {
        if (slot.segment_id == 0) {
            continue;
        }
        if (!slot.entry && !slot.frame) {
            return true;
        }
        if ((slot.entry && slot.entry->info.summary.task_id == task) ||
            (slot.frame && slot.frame->sealed.segment &&
             slot.frame->sealed.segment->metadata.task_id() == task)) {
            return true;
        }
    }
    for (const auto& item : state->quarantined) {
        if (!item || !item->info.task_id || *item->info.task_id == task) {
            return true;
        }
    }
    return false;
}

Result<std::vector<ArtifactId>> ArtifactCatalog::reset_targets(
    TaskId task, ControlGeneration max_control_generation) const {
    auto state = state_;
    auto records = state->options.ledger->recovery_snapshot();
    if (!records.ok()) {
        return records.status();
    }
    try {
        std::vector<ArtifactId> targets;
        for (const auto& record : records.value()) {
            if (record.task_id == task && record.control_generation <= max_control_generation &&
                (record.state == LedgerState::Committed || record.state == LedgerState::Deleting)) {
                targets.push_back(ArtifactId{record.segment_id});
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

Result<void> ArtifactCatalog::purge_quarantined_for_reset(
    TaskId task, ControlGeneration max_control_generation) {
    auto state = state_;
    auto valid = state->options.volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    struct Target {
        std::string name;
        bool in_quarantine_dir{true};
    };
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(state->index);
        if (state->rebuilding) {
            return Status(StatusCode::Conflict, {});
        }
        for (const auto& item : state->quarantined) {
            if (item && item->info.task_id && *item->info.task_id == task &&
                item->info.control_generation &&
                *item->info.control_generation <= max_control_generation &&
                item->info.reason != QuarantineReason::DeletedArtifactReappeared) {
                targets.push_back(Target{item->info.name, item->info.in_quarantine_dir});
            }
        }
    }
    if (targets.empty()) {
        return {};
    }
    FileDescriptor quarantine_dir(open(
        (state->options.volume->root() / ".rodaq/quarantine").c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    FileDescriptor artifacts_dir(open(
        state->options.volume->artifacts_directory().c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (quarantine_dir.get() < 0 || artifacts_dir.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    bool touched_quarantine = false;
    bool touched_artifacts = false;
    for (const auto& target : targets) {
        const int parent_fd = target.in_quarantine_dir ? quarantine_dir.get() : artifacts_dir.get();
        std::uint32_t remaining = kMaxPurgeEntries;
        auto removed =
            remove_tree_bounded(parent_fd, target.name.c_str(), kMaxPurgeDepth, remaining);
        if (!removed.ok()) {
            return removed.status();
        }
        if (target.in_quarantine_dir) {
            touched_quarantine = true;
        } else {
            touched_artifacts = true;
        }
    }
    if ((touched_quarantine && fsync(quarantine_dir.get()) != 0) ||
        (touched_artifacts && fsync(artifacts_dir.get()) != 0)) {
        return Status(StatusCode::Unavailable, {});
    }
    valid = state->options.volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    std::lock_guard<std::mutex> lock(state->index);
    for (const auto& target : targets) {
        auto it = std::find_if(
            state->quarantined.begin(), state->quarantined.end(), [&](const auto& entry) {
                return entry && entry->info.name == target.name &&
                       entry->info.in_quarantine_dir == target.in_quarantine_dir;
            });
        if (it != state->quarantined.end()) {
            if ((*it)->charge) {
                auto deleted = (*it)->charge->release_after_delete();
                if (!deleted.ok()) {
                    return deleted.status();
                }
                auto collected = (*it)->charge->release_after_gc();
                if (!collected.ok()) {
                    return collected.status();
                }
            }
            state->quarantined.erase(it);
        }
    }
    return {};
}

CatalogSnapshot ArtifactCatalog::snapshot() const {
    auto state = state_;
    CatalogSnapshot result;
    std::lock_guard<std::mutex> lock(state->index);
    result.protection = state->options.volume->protection() == StatusCode::Ok
                            ? state->options.ledger->protection()
                            : state->options.volume->protection();
    if (result.protection == StatusCode::Ok && state->recovery_health != StatusCode::Ok) {
        result.protection = state->recovery_health;
    }
    for (const auto& slot : state->slots) {
        auto public_fact = state->public_entry(slot);
        const bool normal = public_fact.ok() && public_fact.value();
        if (!public_fact.ok() && result.protection == StatusCode::Ok) {
            result.protection = public_fact.status().code();
        }
        if (slot.failure != StatusCode::Ok) {
            if (result.protection == StatusCode::Ok) {
                result.protection = slot.failure;
            }
            if (slot.failure == StatusCode::DataLoss && !normal) {
                ++result.corrupted_committed;
            }
        }
        result.visible += normal ? 1 : 0;
        bool deleted = slot.collection_pending;
        if (slot.segment_id != 0 && !normal) {
            auto record = state->options.ledger->get(slot.segment_id);
            deleted = deleted || (record.ok() && record.value().state == LedgerState::Deleted);
        }
        result.pending += slot.segment_id != 0 && !normal && !deleted ? 1 : 0;
        result.commit_inflight += slot.work == CatalogWork::Commit ? 1 : 0;
        result.delete_inflight += slot.work == CatalogWork::Delete ? 1 : 0;
        result.collect_inflight += slot.work == CatalogWork::Collect ? 1 : 0;
    }
    for (const auto& item : state->quarantined) {
        if (!item) {
            continue;
        }
        ++result.quarantined;
        if (!item->info.in_quarantine_dir) {
            ++result.verification_required;
        }
        result.quarantined_bytes += item->info.physical_bytes;
    }
    result.read_leases = state->read_leases;
    result.reads_inflight = state->active_reads.load();
    return result;
}

}  // namespace rodaq::artifact
