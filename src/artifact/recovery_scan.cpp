#include "recovery_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

#include "catalog_state.hpp"
#include "../persistence/decimal.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

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

}  // namespace rodaq::artifact
