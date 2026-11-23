#include "catalog_state.hpp"

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

#include "manifest.hpp"
#include "../persistence/digest.hpp"
#include "../persistence/file_descriptor.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

bool same_times(const struct stat& expected, const struct stat& actual) noexcept {
    return expected.st_mtim.tv_sec == actual.st_mtim.tv_sec &&
           expected.st_mtim.tv_nsec == actual.st_mtim.tv_nsec &&
           expected.st_ctim.tv_sec == actual.st_ctim.tv_sec &&
           expected.st_ctim.tv_nsec == actual.st_ctim.tv_nsec;
}

Result<std::string> manifest_bytes(int fd, std::size_t max_bytes, struct stat& identity) {
    if (fstat(fd, &identity) != 0 || !S_ISREG(identity.st_mode) || identity.st_nlink != 1 ||
        identity.st_size < 0 || static_cast<std::uint64_t>(identity.st_size) > max_bytes) {
        return Status(StatusCode::DataLoss, {});
    }
    std::string bytes(static_cast<std::size_t>(identity.st_size), '\0');
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const auto received =
            pread(fd, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return Status(StatusCode::DataLoss, {});
        }
        offset += static_cast<std::size_t>(received);
    }
    struct stat after {};
    if (fstat(fd, &after) != 0 || !same_regular_file(identity, after)) {
        return Status(StatusCode::DataLoss, {});
    }
    return bytes;
}

Result<void> exact_files(int directory, const std::vector<CatalogFile>& files) {
    FileDescriptor scan(openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (scan.get() < 0) {
        return Status(StatusCode::DataLoss, {});
    }
    const auto close_directory = [](DIR* directory) noexcept { (void)closedir(directory); };
    std::unique_ptr<DIR, decltype(close_directory)> entries(fdopendir(scan.get()), close_directory);
    if (!entries) {
        return Status(StatusCode::Unavailable, {});
    }
    (void)scan.release();
    std::size_t count = 0;
    errno = 0;
    while (const auto* entry = readdir(entries.get())) {
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        if (++count > files.size() + 1 ||
            (name != "manifest.json" && std::none_of(
                                            files.begin(), files.end(),
                                            [&](const auto& file) { return file.path == name; }))) {
            return Status(StatusCode::DataLoss, {});
        }
        errno = 0;
    }
    return errno == 0 && count == files.size() + 1 ? Result<void>{}
                                                   : Result<void>(Status(StatusCode::DataLoss, {}));
}

}  // namespace

bool same_regular_file(const struct stat& expected, const struct stat& actual) noexcept {
    return S_ISREG(actual.st_mode) && actual.st_nlink == 1 && expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino && expected.st_size == actual.st_size &&
           same_times(expected, actual);
}

bool same_segment_directory(const struct stat& expected, const struct stat& actual) noexcept {
    return S_ISDIR(actual.st_mode) && expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino && same_times(expected, actual);
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
        return Status(StatusCode::DataLoss, {});
    }
    FileDescriptor directory(openat(
        parent.get(), std::to_string(segment).c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) {
        return Status(StatusCode::DataLoss, {});
    }
    valid = volume->verify();
    return valid.ok() ? Result<FileDescriptor>(std::move(directory))
                      : Result<FileDescriptor>(valid.status());
}

Result<std::shared_ptr<CatalogEntry>> verify_committed(
    const CatalogOptions& options, const LedgerRecord& record) {
    if (record.state != LedgerState::Committed || !record.committed_at ||
        !(record.volume_id == options.volume->identity())) {
        return Status(StatusCode::InvalidState, {});
    }
    try {
        auto opened = open_final_directory(options.volume, record.segment_id);
        if (!opened.ok()) {
            return opened.status();
        }
        auto entry = std::make_shared<CatalogEntry>();
        const int directory = opened.value().get();
        if (fstat(directory, &entry->directory) != 0) {
            return Status(StatusCode::DataLoss, {});
        }
        FileDescriptor input(
            openat(directory, "manifest.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (input.get() < 0) {
            return Status(StatusCode::DataLoss, {});
        }
        auto bytes = manifest_bytes(input.get(), options.manifest.max_bytes, entry->manifest);
        if (!bytes.ok()) {
            return bytes.status();
        }
        auto decoded = decode_manifest(bytes.value(), options.manifest);
        if (!decoded.ok()) {
            return decoded.status();
        }
        auto& manifest = decoded.value();
        auto digest = manifest_digest(manifest, options.manifest);
        if (!digest.ok()) {
            return digest.status();
        }
        const auto& identity = manifest.identity;
        if (digest.value() != record.manifest_digest || identity.segment_id != record.segment_id ||
            !(identity.task_id == record.task_id) || identity.pipeline_id != record.pipeline_id ||
            !(identity.volume_id == record.volume_id) ||
            identity.control_generation != record.control_generation ||
            identity.reset_generation_at_commit != record.reset_generation_at_commit) {
            return Status(StatusCode::DataLoss, {});
        }
        entry->config_version = identity.config_version;
        auto& summary = entry->info.summary;
        summary.artifact_id = {record.segment_id};
        summary.segment_id = record.segment_id;
        summary.task_id = record.task_id;
        summary.pipeline_id = record.pipeline_id;
        summary.committed_at = *record.committed_at;
        summary.recording_range = {manifest.recording_begin_ns, manifest.recording_end_ns};
        summary.receive_wall_time_range = {
            WallTime{std::chrono::nanoseconds{manifest.min_receive_realtime}},
            WallTime{std::chrono::nanoseconds{manifest.max_receive_realtime}}};
        summary.format = identity.format;
        summary.logical_bytes = manifest.logical_bytes;
        summary.physical_bytes = static_cast<std::uint64_t>(entry->manifest.st_size);
        bool has_written = false;
        for (const auto& coverage : manifest.coverage) {
            if (coverage.unknown != 0) {
                return Status(StatusCode::DataLoss, {});
            }
            if (coverage.known_failed != 0) {
                summary.data_impact = DataImpact::AcceptedKnownFailed;
            }
            has_written = has_written || coverage.written != 0;
            entry->info.coverage.push_back(
                {coverage.stream_id, coverage.accepted, coverage.written, coverage.known_failed,
                 coverage.unknown, coverage.first_sequence, coverage.last_sequence});
        }
        if (!has_written) {
            return Status(StatusCode::DataLoss, {});
        }
        std::sort(manifest.files.begin(), manifest.files.end(), [](const auto& a, const auto& b) {
            return a.relative_path < b.relative_path;
        });
        for (const auto& file : manifest.files) {
            FileDescriptor descriptor(openat(
                directory, file.relative_path.c_str(),
                O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            CatalogFile observed;
            observed.path = file.relative_path;
            if (descriptor.get() < 0 || fstat(descriptor.get(), &observed.identity) != 0 ||
                !S_ISREG(observed.identity.st_mode) || observed.identity.st_nlink != 1 ||
                observed.identity.st_size < 0 ||
                static_cast<std::uint64_t>(observed.identity.st_size) != file.physical_size ||
                file.physical_size >
                    std::numeric_limits<std::uint64_t>::max() - summary.physical_bytes) {
                return Status(StatusCode::DataLoss, {});
            }
            auto actual = persistence::file_digest(descriptor.get(), file.physical_size);
            if (!actual.ok()) {
                return actual.status();
            }
            if (actual.value() != file.digest) {
                return Status(StatusCode::DataLoss, {});
            }
            struct stat unchanged {};
            if (fstat(descriptor.get(), &unchanged) != 0 ||
                !same_regular_file(observed.identity, unchanged)) {
                return Status(StatusCode::DataLoss, {});
            }
            summary.physical_bytes += file.physical_size;
            entry->info.files.push_back(
                {{static_cast<std::uint32_t>(entry->files.size() + 1)},
                 file.relative_path,
                 file.physical_size,
                 "application/octet-stream",
                 file.digest});
            entry->files.push_back(std::move(observed));
        }
        auto exact = exact_files(directory, entry->files);
        struct stat after {};
        struct stat manifest_after {};
        if (!exact.ok() || fstat(input.get(), &manifest_after) != 0 ||
            !same_regular_file(entry->manifest, manifest_after) || fstat(directory, &after) != 0 ||
            !same_segment_directory(entry->directory, after)) {
            return Status(StatusCode::DataLoss, {});
        }
        auto volume = options.volume->verify();
        return volume.ok() ? Result<std::shared_ptr<CatalogEntry>>(std::move(entry))
                           : Result<std::shared_ptr<CatalogEntry>>(volume.status());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
