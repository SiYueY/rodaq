#include "recovery_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog_state.hpp"
#include "../persistence/decimal.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

constexpr std::uint32_t kMaxRecoveryEntries = 65536;
constexpr std::uint32_t kMaxTreeDepth = 4;
constexpr std::uint32_t kMaxTreeEntries = 1024;

bool add_bytes(std::uint64_t& target, std::uint64_t delta) noexcept {
    if (delta > std::numeric_limits<std::uint64_t>::max() - target) {
        return false;
    }
    target += delta;
    return true;
}

bool measure_tree_bytes(
    int parent_fd, const char* name, std::uint32_t depth, std::uint32_t& remaining_entries,
    std::uint64_t& total_bytes, bool& complete) {
    if (remaining_entries == 0) {
        complete = false;
        return true;
    }
    --remaining_entries;
    struct stat info {};
    if (fstatat(parent_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        complete = false;
        return true;
    }
    if (S_ISREG(info.st_mode)) {
        if (info.st_size < 0 || info.st_nlink != 1) {
            complete = false;
            return true;
        }
        return add_bytes(total_bytes, static_cast<std::uint64_t>(info.st_size));
    }
    if (!S_ISDIR(info.st_mode) || depth == 0) {
        complete = false;
        return true;
    }
    FileDescriptor opened(openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (opened.get() < 0) {
        complete = false;
        return true;
    }
    const auto close_dir = [](DIR* value) noexcept { (void)closedir(value); };
    std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(opened.get()), close_dir);
    if (!entries) {
        complete = false;
        return true;
    }
    (void)opened.release();
    const int child_fd = dirfd(entries.get());
    errno = 0;
    while (const auto* entry = readdir(entries.get())) {
        const std::string_view child(entry->d_name);
        if (child == "." || child == "..") {
            continue;
        }
        if (!measure_tree_bytes(
                child_fd, entry->d_name, depth - 1, remaining_entries, total_bytes, complete)) {
            return false;
        }
        errno = 0;
    }
    if (errno != 0) {
        complete = false;
    }
    return true;
}

// Parses "<uuid>.g<ctrl>.r<reset>.s<seg>[.precommit].(tmp|final)".
bool parse_quarantine_name(std::string_view name, QuarantinedArtifact& out) {
    if (name.size() <= 38 || name[36] != '.' || name[37] != 'g') {
        return false;
    }
    auto task = parse_task_id(name.substr(0, 36));
    if (!task.ok()) {
        return false;
    }
    std::size_t pos = 38;
    const auto next_dot = [&](std::size_t start) { return name.find('.', start); };
    const auto r_dot = next_dot(pos);
    if (r_dot == std::string_view::npos || r_dot + 2 >= name.size() || name[r_dot + 1] != 'r') {
        return false;
    }
    auto control_gen = persistence::unsigned_decimal(name.substr(pos, r_dot - pos));
    if (!control_gen.ok()) {
        return false;
    }
    pos = r_dot + 2;
    const auto s_dot = next_dot(pos);
    if (s_dot == std::string_view::npos || s_dot + 2 >= name.size() || name[s_dot + 1] != 's') {
        return false;
    }
    auto reset_gen = persistence::unsigned_decimal(name.substr(pos, s_dot - pos));
    if (!reset_gen.ok()) {
        return false;
    }
    pos = s_dot + 2;
    const auto tail_dot = next_dot(pos);
    if (tail_dot == std::string_view::npos) {
        return false;
    }
    auto segment_id = persistence::unsigned_decimal(name.substr(pos, tail_dot - pos));
    if (!segment_id.ok() || segment_id.value() == 0) {
        return false;
    }
    const auto suffix = name.substr(tail_dot);
    if (suffix == ".tmp" || suffix == ".final") {
        out.reason = QuarantineReason::ResetInvalidated;
    } else if (suffix == ".precommit.tmp" || suffix == ".precommit.final") {
        out.reason = QuarantineReason::PrecommitFailure;
    } else {
        return false;
    }
    out.task_id = task.value();
    out.control_generation = control_gen.value();
    out.reset_generation = reset_gen.value();
    out.segment_id = segment_id.value();
    return true;
}

struct BootAnchorAccumulator {
    std::uint64_t current_boot_id{0};
    std::optional<time::PlaybackAnchor> anchor;
    bool has_unverified{false};

    Result<void> record_manifest(const SegmentManifest& manifest) {
        const auto& observed = manifest.identity.playback_anchor;
        if (observed.boot_id != current_boot_id) {
            return {};
        }
        if (!anchor) {
            anchor = observed;
            return {};
        }
        if (anchor->monotonic_ns != observed.monotonic_ns ||
            anchor->realtime_ns != observed.realtime_ns) {
            return Status(StatusCode::DataLoss, {});
        }
        return {};
    }

    time::BootArtifactEvidence finalize() const noexcept {
        if (anchor) {
            return {time::BootArtifactState::Present, *anchor};
        }
        if (has_unverified) {
            return {time::BootArtifactState::Unknown, {}};
        }
        return {time::BootArtifactState::Absent, {}};
    }
};

Result<std::uint32_t> inspect_names(
    const std::filesystem::path& path, const std::vector<LedgerRecord>& records) {
    FileDescriptor directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto close = [](DIR* value) noexcept { (void)closedir(value); };
    std::unique_ptr<DIR, decltype(close)> entries(fdopendir(directory.get()), close);
    if (!entries) {
        return Status(StatusCode::Unavailable, {});
    }
    (void)directory.release();
    std::uint32_t count = 0;
    errno = 0;
    while (const auto* entry = readdir(entries.get())) {
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (count == records.size()) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto id = persistence::unsigned_decimal(name);
        if (!id.ok() || id.value() == 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto found = std::lower_bound(
            records.begin(), records.end(), id.value(),
            [](const auto& record, std::uint64_t segment) { return record.segment_id < segment; });
        if (found == records.end() || found->segment_id != id.value()) {
            return Status(StatusCode::Unavailable, {});
        }
        if (found->state == LedgerState::Deleted) {
            return Status(StatusCode::DataLoss, {});
        }
        ++count;
        errno = 0;
    }
    return errno == 0 ? Result<std::uint32_t>(count)
                      : Result<std::uint32_t>(Status(StatusCode::Unavailable, {}));
}

}  // namespace

Result<RecoveredVolumeUsage> inspect_committed_volume(
    const std::shared_ptr<ManagedVolume>& volume, const LedgerStore& ledger,
    const ManifestLimits& limits) {
    if (!volume || !validate_manifest_limits(limits).ok()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto valid = volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    try {
        auto snapshot = ledger.recovery_snapshot();
        if (!snapshot.ok()) {
            return snapshot.status();
        }
        auto& records = snapshot.value();
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
            return a.segment_id < b.segment_id;
        });
        std::uint32_t committed = 0;
        for (const auto& record : records) {
            if (record.state != LedgerState::Committed && record.state != LedgerState::Deleted) {
                return Status(StatusCode::Unavailable, {});
            }
            committed += record.state == LedgerState::Committed ? 1 : 0;
        }
        auto names = inspect_names(volume->artifacts_directory(), records);
        if (!names.ok()) {
            return names.status();
        }
        // An empty allowed set rejects the first quarantine entry without any
        // recursive traversal or permission to erase its possible references.
        auto quarantine = inspect_names(volume->root() / ".rodaq/quarantine", {});
        if (!quarantine.ok()) {
            return quarantine.status();
        }
        if (names.value() != committed) {
            return Status(StatusCode::DataLoss, {});
        }
        CatalogOptions options;
        options.volume = volume;
        options.manifest = limits;
        RecoveredVolumeUsage usage;
        for (const auto& record : records) {
            if (record.state == LedgerState::Deleted) {
                if (!(record.volume_id == volume->identity())) {
                    return Status(StatusCode::DataLoss, {});
                }
                auto absent = prove_artifact_absence(options, record.segment_id);
                if (!absent.ok()) {
                    return absent.status();
                }
                ++usage.artifact_slots;
                continue;
            }
            auto verified = verify_committed(options, record);
            if (!verified.ok()) {
                return verified.status();
            }
            const auto bytes = verified.value()->info.summary.physical_bytes;
            if (bytes > std::numeric_limits<std::uint64_t>::max() - usage.physical_bytes) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            usage.physical_bytes += bytes;
            ++usage.artifact_slots;
        }
        valid = volume->verify();
        return valid.ok() ? Result<RecoveredVolumeUsage>(usage)
                          : Result<RecoveredVolumeUsage>(valid.status());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<VolumeRecoveryReport> inspect_volume_recovery(
    const std::shared_ptr<ManagedVolume>& volume, const LedgerStore& ledger,
    const ManifestLimits& limits, const time::BootIdentity& current_boot) {
    if (!volume || !validate_manifest_limits(limits).ok()) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto verified_boot = time::parse_boot_identity(current_boot.kernel_uuid);
    if (!verified_boot.ok() || verified_boot.value().id != current_boot.id) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto valid = volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    try {
        auto snapshot = ledger.recovery_snapshot();
        if (!snapshot.ok()) {
            return snapshot.status();
        }
        auto& records = snapshot.value();
        std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
            return a.segment_id < b.segment_id;
        });
        for (const auto& record : records) {
            if (record.state != LedgerState::Committed && record.state != LedgerState::Deleted) {
                return Status(StatusCode::Unavailable, {});
            }
            if (!(record.volume_id == volume->identity())) {
                return Status(StatusCode::DataLoss, {});
            }
        }

        CatalogOptions options;
        options.volume = volume;
        options.manifest = limits;
        VolumeRecoveryReport report;
        BootAnchorAccumulator boot_anchors{current_boot.id, {}, false};

        for (const auto& record : records) {
            if (record.state == LedgerState::Deleted) {
                ++report.deleted_count;
                ++report.artifact_slots;
                continue;
            }
            const auto final_path =
                volume->artifacts_directory() / std::to_string(record.segment_id);
            auto tree = verify_artifact_tree(options, final_path);
            if (!tree.ok()) {
                if (tree.status().code() == StatusCode::ResourceExhausted) {
                    return tree.status();
                }
                ++report.corrupted_committed_count;
                ++report.artifact_slots;
                report.corrupted_committed_segments.push_back(record.segment_id);
                report.usage_complete = false;
                report.health = StatusCode::DataLoss;
                boot_anchors.has_unverified = true;
                continue;
            }
            auto binding = validate_manifest_ledger_binding(tree.value().manifest, record, limits);
            if (!binding.ok()) {
                ++report.corrupted_committed_count;
                ++report.artifact_slots;
                report.corrupted_committed_segments.push_back(record.segment_id);
                report.usage_complete = false;
                report.health = StatusCode::DataLoss;
                boot_anchors.has_unverified = true;
                continue;
            }
            auto anchor_ok = boot_anchors.record_manifest(tree.value().manifest);
            if (!anchor_ok.ok()) {
                return anchor_ok.status();
            }
            const auto bytes = tree.value().entry->info.summary.physical_bytes;
            if (!add_bytes(report.committed_bytes, bytes)) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            ++report.committed_count;
            ++report.artifact_slots;
        }

        const auto inspect_unknown_entry =
            [&](int parent_fd, const std::filesystem::path& dir_path, std::string_view name,
                bool in_quarantine, QuarantineReason default_reason,
                std::optional<TaskId> known_task,
                std::optional<std::uint64_t> known_segment) -> Result<void> {
            if (report.artifact_slots >= kMaxRecoveryEntries) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            QuarantinedArtifact item;
            item.name = std::string(name);
            item.in_quarantine_dir = in_quarantine;
            item.reason = default_reason;
            item.task_id = known_task;
            item.segment_id = known_segment;
            const bool parsed_name = in_quarantine && parse_quarantine_name(name, item);

            auto tree = verify_artifact_tree(options, dir_path / item.name);
            if (tree.ok()) {
                const auto& identity = tree.value().manifest.identity;
                if (parsed_name && (!(identity.task_id == *item.task_id) ||
                                    identity.segment_id != *item.segment_id)) {
                    item.task_id.reset();
                    report.health = StatusCode::DataLoss;
                    boot_anchors.has_unverified = true;
                } else {
                    if (!item.task_id) {
                        item.task_id = identity.task_id;
                    }
                    if (!item.control_generation) {
                        item.control_generation = identity.control_generation;
                    }
                    if (!item.reset_generation) {
                        item.reset_generation = identity.reset_generation_at_commit;
                    }
                    if (!item.segment_id) {
                        item.segment_id = identity.segment_id;
                    }
                    auto anchor_ok = boot_anchors.record_manifest(tree.value().manifest);
                    if (!anchor_ok.ok()) {
                        return anchor_ok.status();
                    }
                }
                item.physical_bytes = tree.value().entry->info.summary.physical_bytes;
                item.bytes_complete = true;
            } else {
                if (tree.status().code() == StatusCode::ResourceExhausted) {
                    return tree.status();
                }
                std::uint32_t remaining = kMaxTreeEntries;
                std::uint64_t measured = 0;
                bool complete = true;
                if (!measure_tree_bytes(
                        parent_fd, item.name.c_str(), kMaxTreeDepth, remaining, measured,
                        complete)) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                item.physical_bytes = measured;
                item.bytes_complete = complete;
                if (!complete) {
                    report.usage_complete = false;
                    report.health = StatusCode::DataLoss;
                }
                boot_anchors.has_unverified = true;
            }

            if (!add_bytes(report.quarantined_bytes, item.physical_bytes)) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            if (!in_quarantine) {
                ++report.verification_required_count;
            }
            if (item.reason == QuarantineReason::DeletedArtifactReappeared) {
                report.health = StatusCode::DataLoss;
            }
            ++report.artifact_slots;
            report.quarantined.push_back(std::move(item));
            return {};
        };

        // Scan .rodaq/artifacts for Ledger-absent entries and reappeared Deleted entries.
        {
            const auto& artifacts_path = volume->artifacts_directory();
            FileDescriptor dir_fd(
                open(artifacts_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (dir_fd.get() < 0) {
                return Status(StatusCode::Unavailable, {});
            }
            const auto close_dir = [](DIR* value) noexcept { (void)closedir(value); };
            std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(dir_fd.get()), close_dir);
            if (!entries) {
                return Status(StatusCode::Unavailable, {});
            }
            (void)dir_fd.release();
            const int parent_fd = dirfd(entries.get());
            std::uint32_t scanned = 0;
            errno = 0;
            while (const auto* entry = readdir(entries.get())) {
                const std::string_view name(entry->d_name);
                if (name == "." || name == "..") {
                    continue;
                }
                if (++scanned > kMaxRecoveryEntries) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                const bool is_tmp =
                    name.size() > 4 && name.substr(name.size() - 4) == std::string_view(".tmp");
                const auto stem = is_tmp ? name.substr(0, name.size() - 4) : name;
                const auto parsed_id = persistence::unsigned_decimal(stem);
                if (parsed_id.ok() && parsed_id.value() != 0) {
                    const auto found = std::lower_bound(
                        records.begin(), records.end(), parsed_id.value(),
                        [](const auto& rec, std::uint64_t seg) { return rec.segment_id < seg; });
                    if (found != records.end() && found->segment_id == parsed_id.value()) {
                        if (found->state == LedgerState::Committed && !is_tmp) {
                            errno = 0;
                            continue;
                        }
                        const auto reason = found->state == LedgerState::Deleted
                                                ? QuarantineReason::DeletedArtifactReappeared
                                                : QuarantineReason::LedgerAbsentStaging;
                        auto classified = inspect_unknown_entry(
                            parent_fd, artifacts_path, name, false, reason, found->task_id,
                            found->segment_id);
                        if (!classified.ok()) {
                            return classified.status();
                        }
                        errno = 0;
                        continue;
                    }
                    const auto reason = is_tmp ? QuarantineReason::LedgerAbsentStaging
                                               : QuarantineReason::LedgerAbsentFinal;
                    auto classified = inspect_unknown_entry(
                        parent_fd, artifacts_path, name, false, reason, std::nullopt,
                        parsed_id.value());
                    if (!classified.ok()) {
                        return classified.status();
                    }
                } else {
                    auto classified = inspect_unknown_entry(
                        parent_fd, artifacts_path, name, false, QuarantineReason::UnknownEntry,
                        std::nullopt, std::nullopt);
                    if (!classified.ok()) {
                        return classified.status();
                    }
                }
                errno = 0;
            }
            if (errno != 0) {
                return Status(StatusCode::Unavailable, {});
            }
        }

        // Scan .rodaq/quarantine for retained quarantine trees.
        {
            const auto quarantine_path = volume->root() / ".rodaq/quarantine";
            FileDescriptor dir_fd(
                open(quarantine_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (dir_fd.get() < 0) {
                return Status(StatusCode::Unavailable, {});
            }
            const auto close_dir = [](DIR* value) noexcept { (void)closedir(value); };
            std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(dir_fd.get()), close_dir);
            if (!entries) {
                return Status(StatusCode::Unavailable, {});
            }
            (void)dir_fd.release();
            const int parent_fd = dirfd(entries.get());
            std::uint32_t scanned = 0;
            errno = 0;
            while (const auto* entry = readdir(entries.get())) {
                const std::string_view name(entry->d_name);
                if (name == "." || name == "..") {
                    continue;
                }
                if (++scanned > kMaxRecoveryEntries) {
                    return Status(StatusCode::ResourceExhausted, {});
                }
                auto classified = inspect_unknown_entry(
                    parent_fd, quarantine_path, name, true, QuarantineReason::UnknownEntry,
                    std::nullopt, std::nullopt);
                if (!classified.ok()) {
                    return classified.status();
                }
                errno = 0;
            }
            if (errno != 0) {
                return Status(StatusCode::Unavailable, {});
            }
        }

        if (report.committed_bytes >
            std::numeric_limits<std::uint64_t>::max() - report.quarantined_bytes) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        report.boot_evidence = boot_anchors.finalize();
        valid = volume->verify();
        return valid.ok() ? Result<VolumeRecoveryReport>(std::move(report))
                          : Result<VolumeRecoveryReport>(valid.status());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
