#include "startup_scan.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <rodaq/logging.hpp>

#include "../persistence/atomic_file.hpp"
#include "../persistence/decimal.hpp"
#include "../persistence/file_descriptor.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

std::string media_type_for(std::string_view relative_path) {
    if (relative_path.size() >= 5 && relative_path.substr(relative_path.size() - 5) == ".json") {
        return "application/json";
    }
    return "application/octet-stream";
}

std::string make_etag(
    std::uint64_t segment_id, std::uint32_t file_id, std::uint64_t physical_size) {
    return "v1-" + std::to_string(segment_id) + "-" + std::to_string(file_id) + "-" +
           std::to_string(physical_size);
}

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool parse_segment_name(std::string_view stem, std::uint64_t& segment_id) {
    auto parsed = persistence::unsigned_decimal(stem);
    if (!parsed.ok() || parsed.value() == 0) {
        return false;
    }
    segment_id = parsed.value();
    return true;
}

}  // namespace

bool same_regular_file(const struct stat& expected, const struct stat& actual) noexcept {
    return S_ISREG(actual.st_mode) && actual.st_nlink == 1 && expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino && expected.st_size == actual.st_size &&
           expected.st_mtim.tv_sec == actual.st_mtim.tv_sec &&
           expected.st_mtim.tv_nsec == actual.st_mtim.tv_nsec;
}

bool same_segment_directory(const struct stat& expected, const struct stat& actual) noexcept {
    return S_ISDIR(actual.st_mode) && expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino;
}

Result<FileDescriptor> open_final_directory(
    const std::shared_ptr<ManagedVolume>& volume, std::uint64_t segment) {
    auto valid = volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    FileDescriptor parent(open(
        volume->artifacts_directory().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    const auto name = std::to_string(segment);
    FileDescriptor directory(
        openat(parent.get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) {
        if (errno == ENOENT || errno == ELOOP || errno == ENOTDIR) {
            return Status(StatusCode::DataLoss, {});
        }
        return Status(StatusCode::Unavailable, {});
    }
    return directory;
}

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
    if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) {
        return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT
                   ? Result<void>{}
                   : Result<void>(Status(StatusCode::Unavailable, {}));
    }
    if (!S_ISDIR(info.st_mode) || depth == 0) {
        return Status(StatusCode::DataLoss, {});
    }
    FileDescriptor opened(openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (opened.get() < 0) {
        return errno == ENOENT ? Result<void>{} : Result<void>(Status(StatusCode::Unavailable, {}));
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
    entries.reset();
    if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
        return Status(StatusCode::Unavailable, {});
    }
    return {};
}

Result<std::shared_ptr<CatalogEntry>> verify_final_artifact(
    const CatalogOptions& options, std::uint64_t segment_id) {
    try {
        auto directory = open_final_directory(options.volume, segment_id);
        if (!directory.ok()) {
            return directory.status();
        }
        struct stat dir_stat {};
        if (fstat(directory.value().get(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
            return Status(StatusCode::DataLoss, {});
        }
        FileDescriptor manifest_fd(openat(
            directory.value().get(), "manifest.json",
            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (manifest_fd.get() < 0) {
            return (errno == ENOENT || errno == ELOOP || errno == ENOTDIR)
                       ? Result<std::shared_ptr<CatalogEntry>>(Status(StatusCode::DataLoss, {}))
                       : Result<std::shared_ptr<CatalogEntry>>(Status(StatusCode::Unavailable, {}));
        }
        struct stat manifest_stat {};
        if (fstat(manifest_fd.get(), &manifest_stat) != 0 || !S_ISREG(manifest_stat.st_mode) ||
            manifest_stat.st_nlink != 1 || manifest_stat.st_size <= 0 ||
            static_cast<std::uint64_t>(manifest_stat.st_size) > options.manifest.max_bytes) {
            return Status(StatusCode::DataLoss, {});
        }
        const auto artifact_dir =
            options.volume->artifacts_directory() / std::to_string(segment_id);
        auto bytes =
            persistence::read_file(artifact_dir, "manifest.json", options.manifest.max_bytes);
        if (!bytes.ok()) {
            return bytes.status().code() == StatusCode::NotFound ? Status(StatusCode::DataLoss, {})
                                                                 : bytes.status();
        }
        auto manifest = decode_manifest(bytes.value(), options.manifest);
        if (!manifest.ok()) {
            return manifest.status();
        }
        const auto& m = manifest.value();
        if (m.identity.segment_id != segment_id ||
            !(m.identity.volume_id == options.volume->identity())) {
            return Status(StatusCode::DataLoss, {});
        }
        // Reject any unexpected entries in the flat artifact directory.
        FileDescriptor scan_fd(
            openat(directory.value().get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (scan_fd.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto close_dir = [](DIR* value) noexcept { (void)closedir(value); };
        std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(scan_fd.get()), close_dir);
        if (!entries) {
            return Status(StatusCode::Unavailable, {});
        }
        (void)scan_fd.release();
        std::vector<std::string> observed_names;
        errno = 0;
        while (const auto* item = readdir(entries.get())) {
            const std::string_view name(item->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            if (observed_names.size() >= m.files.size() + 1) {
                return Status(StatusCode::DataLoss, {});
            }
            observed_names.emplace_back(name);
            errno = 0;
        }
        if (errno != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        if (observed_names.size() != m.files.size() + 1) {
            return Status(StatusCode::DataLoss, {});
        }

        auto entry = std::make_shared<CatalogEntry>();
        entry->directory = dir_stat;
        entry->manifest = manifest_stat;
        entry->config_version = m.identity.config_version;

        // Sort files bytewise by relative_path to assign stable 1-based ArtifactFileId.
        std::vector<FileEvidence> sorted_files = m.files;
        std::sort(sorted_files.begin(), sorted_files.end(), [](const auto& a, const auto& b) {
            return a.relative_path < b.relative_path;
        });

        std::uint64_t total_physical = static_cast<std::uint64_t>(manifest_stat.st_size);
        entry->files.reserve(sorted_files.size());
        entry->info.files.reserve(sorted_files.size());

        for (std::size_t i = 0; i < sorted_files.size(); ++i) {
            const auto& file = sorted_files[i];
            struct stat file_stat {};
            if (fstatat(
                    directory.value().get(), file.relative_path.c_str(), &file_stat,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(file_stat.st_mode) || file_stat.st_nlink != 1 || file_stat.st_size < 0 ||
                static_cast<std::uint64_t>(file_stat.st_size) != file.physical_size) {
                return Status(StatusCode::DataLoss, {});
            }
            if (file.physical_size > std::numeric_limits<std::uint64_t>::max() - total_physical) {
                return Status(StatusCode::DataLoss, {});
            }
            total_physical += file.physical_size;
            entry->files.push_back(CatalogFile{file.relative_path, file_stat});
            const auto file_id = static_cast<std::uint32_t>(i + 1);
            entry->info.files.push_back(ArtifactFileInfo{
                ArtifactFileId{file_id},
                file.relative_path,
                file.physical_size,
                media_type_for(file.relative_path),
                make_etag(segment_id, file_id, file.physical_size),
            });
        }

        // Verify all observed names were accounted for (manifest.json + sorted_files).
        for (const auto& name : observed_names) {
            if (name == "manifest.json") {
                continue;
            }
            const bool found = std::any_of(
                sorted_files.begin(), sorted_files.end(),
                [&](const auto& file) { return file.relative_path == name; });
            if (!found) {
                return Status(StatusCode::DataLoss, {});
            }
        }

        auto& summary = entry->info.summary;
        summary.artifact_id = ArtifactId{segment_id};
        summary.task_id = m.identity.task_id;
        summary.pipeline_id = m.identity.pipeline_id;
        summary.segment_id = segment_id;
        summary.committed_at = m.identity.committed_at;
        summary.recording_range = {m.recording_begin_ns, m.recording_end_ns};
        summary.receive_wall_time_range = {
            WallTime{std::chrono::nanoseconds{m.min_receive_realtime}},
            WallTime{std::chrono::nanoseconds{m.max_receive_realtime}},
        };
        summary.format = m.identity.format;
        summary.logical_bytes = m.logical_bytes;
        summary.physical_bytes = total_physical;
        summary.data_impact = DataImpact::None;

        entry->info.coverage.reserve(m.coverage.size());
        for (const auto& cov : m.coverage) {
            if (cov.unknown != 0) {
                return Status(StatusCode::DataLoss, {});
            }
            if (cov.known_failed != 0) {
                summary.data_impact = DataImpact::AcceptedKnownFailed;
            }
            entry->info.coverage.push_back(ArtifactStreamCoverage{
                cov.stream_id,
                cov.accepted,
                cov.written,
                cov.known_failed,
                cov.unknown,
                cov.first_sequence,
                cov.last_sequence,
            });
        }
        return entry;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<StartupScanReport> scan_volume(const CatalogOptions& options) {
    if (!options.volume || !validate_manifest_limits(options.manifest).ok() ||
        options.max_artifacts == 0 || options.max_artifacts > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto verified = options.volume->verify();
    if (!verified.ok()) {
        return verified.status();
    }
    try {
        FileDescriptor parent(open(
            options.volume->artifacts_directory().c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        FileDescriptor dup_fd(openat(parent.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (dup_fd.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto close_dir = [](DIR* value) noexcept { (void)closedir(value); };
        std::unique_ptr<DIR, decltype(close_dir)> entries(fdopendir(dup_fd.get()), close_dir);
        if (!entries) {
            return Status(StatusCode::Unavailable, {});
        }
        (void)dup_fd.release();

        std::vector<std::string> names;
        errno = 0;
        while (const auto* item = readdir(entries.get())) {
            const std::string_view name(item->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            if (names.size() >= options.max_artifacts) {
                return Status(
                    StatusCode::ResourceExhausted,
                    "Artifacts directory exceeds max_artifacts_per_volume");
            }
            names.emplace_back(name);
            errno = 0;
        }
        if (errno != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        entries.reset();
        std::sort(names.begin(), names.end());

        StartupScanReport report;
        bool modified_parent = false;
        constexpr std::uint32_t kMaxCleanupDepth = 4;
        constexpr std::uint32_t kMaxCleanupEntries = 1024;

        for (const auto& name : names) {
            std::uint64_t segment_id = 0;
            if (ends_with(name, ".tmp") &&
                parse_segment_name(std::string_view(name).substr(0, name.size() - 4), segment_id)) {
                std::uint32_t remaining = kMaxCleanupEntries;
                auto removed =
                    remove_tree_bounded(parent.get(), name.c_str(), kMaxCleanupDepth, remaining);
                if (!removed.ok()) {
                    logging::write(
                        logging::Level::Error, {},
                        "Startup scan failed to remove stale staging directory: " + name);
                    return removed.status();
                }
                ++report.cleaned_tmp;
                modified_parent = true;
                continue;
            }
            if (ends_with(name, ".deleting") &&
                parse_segment_name(std::string_view(name).substr(0, name.size() - 9), segment_id)) {
                std::uint32_t remaining = kMaxCleanupEntries;
                auto removed =
                    remove_tree_bounded(parent.get(), name.c_str(), kMaxCleanupDepth, remaining);
                if (!removed.ok()) {
                    logging::write(
                        logging::Level::Error, {},
                        "Startup scan failed to remove deleting directory: " + name);
                    return removed.status();
                }
                ++report.cleaned_deleting;
                modified_parent = true;
                continue;
            }
            if (!parse_segment_name(name, segment_id)) {
                ++report.corrupt_count;
                logging::write(
                    logging::Level::Warning, {},
                    "Startup scan ignored unexpected artifacts entry: " + name);
                continue;
            }
            auto entry = verify_final_artifact(options, segment_id);
            if (!entry.ok()) {
                if (entry.status().code() == StatusCode::DataLoss) {
                    ++report.corrupt_count;
                    logging::write(
                        logging::Level::Warning, {},
                        "Startup scan skipped corrupt final artifact: " + name);
                    continue;
                }
                return entry.status();
            }
            const auto bytes = entry.value()->info.summary.physical_bytes;
            if (bytes > std::numeric_limits<std::uint64_t>::max() - report.total_physical_bytes) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            report.total_physical_bytes += bytes;
            report.entries.push_back(std::move(entry.value()));
        }

        if (modified_parent && fsync(parent.get()) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        verified = options.volume->verify();
        if (!verified.ok()) {
            return verified.status();
        }
        return report;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
