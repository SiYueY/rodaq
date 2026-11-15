#include "sealed_files.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <rodaq/storage/validation.hpp>

#include "../persistence/digest.hpp"
#include "../persistence/file_descriptor.hpp"

namespace rodaq::artifact {
namespace {

using persistence::FileDescriptor;

bool same_file(const struct stat& before, const struct stat& after) noexcept {
    return S_ISREG(after.st_mode) && after.st_nlink == 1 && before.st_dev == after.st_dev &&
           before.st_ino == after.st_ino && before.st_size == after.st_size &&
           before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
           before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
           before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
           before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

Result<void> synchronize(
    int fd, persistence::FileStep step, const persistence::FileFaultHook& faults) {
    if (faults) {
        try {
            if (faults(step) != 0) {
                return Status(StatusCode::Unavailable, {});
            }
        } catch (...) {
            return Status(StatusCode::Internal, {});
        }
    }
    int result = 0;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? Result<void>{} : Result<void>{Status(StatusCode::Unavailable, {})};
}

struct OpenedFile {
    FileDescriptor descriptor;
    struct stat identity {};

    OpenedFile(FileDescriptor fd, struct stat attributes) noexcept
    : descriptor(std::move(fd)), identity(attributes) {}
};

}  // namespace

struct SealedFiles::State {
    std::shared_ptr<ManagedVolume> volume;
    std::filesystem::path staging;
    FileDescriptor directory;
    struct stat directory_identity {};
    std::vector<FileEvidence> evidence;
    std::vector<OpenedFile> files;
    std::unique_ptr<OpenedFile> manifest;
    persistence::FileFaultHook faults;
    std::uint64_t storage_bytes{0};
    std::uint64_t manifest_bytes{0};
    std::uint64_t manifest_limit{0};
    bool manifest_attempted{false};
    bool synced{false};

    State(std::shared_ptr<ManagedVolume> owner, std::filesystem::path path, FileDescriptor fd)
    : volume(std::move(owner)), staging(std::move(path)), directory(std::move(fd)) {}

    Result<void> verify_entries() const {
        // openat creates a new directory description: duplicated descriptions
        // share their read offset, so dup+fdopendir would break later scans.
        const int scan = openat(directory.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (scan < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        DIR* raw = fdopendir(scan);
        if (raw == nullptr) {
            close(scan);
            return Status(StatusCode::Unavailable, {});
        }
        const auto close_directory = [](DIR* value) { closedir(value); };
        std::unique_ptr<DIR, decltype(close_directory)> entries(raw, close_directory);
        std::size_t count = 0;
        errno = 0;
        while (const auto* entry = readdir(entries.get())) {
            const std::string_view name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            if (++count > evidence.size() + (manifest ? 1u : 0u)) {
                return Status(StatusCode::DataLoss, {});
            }
            if (manifest && name == "manifest.json") {
                continue;
            }
            if (std::none_of(evidence.begin(), evidence.end(), [&](const auto& file) {
                    return file.relative_path == name;
                })) {
                return Status(StatusCode::DataLoss, {});
            }
            errno = 0;
        }
        if (errno != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        return count == evidence.size() + (manifest ? 1u : 0u)
                   ? Result<void>{}
                   : Result<void>{Status(StatusCode::DataLoss, {})};
    }
};

SealedFiles::SealedFiles(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

SealedFiles::~SealedFiles() noexcept = default;

Result<std::unique_ptr<SealedFiles>> SealedFiles::open(
    std::shared_ptr<ManagedVolume> volume, std::uint64_t segment_id,
    const storage::SegmentSealResult& seal, const storage::PrepareConfig& limits,
    std::uint64_t manifest_byte_limit, persistence::FileFaultHook faults) {
    if (!volume || segment_id == 0 || seal.files.empty() ||
        seal.files.size() > limits.max_segment_file_count || seal.files.size() > 4096 ||
        limits.max_relative_path_bytes == 0 || limits.max_relative_path_bytes > 4096 ||
        manifest_byte_limit == 0 || manifest_byte_limit > 64 * 1024 * 1024 ||
        !storage::abi::valid_sealed_files(seal) ||
        seal.total_physical_bytes > limits.max_segment_physical_bytes) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto valid = volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    try {
        auto staging = volume->artifacts_directory() / (std::to_string(segment_id) + ".tmp");
        FileDescriptor directory(
            ::open(staging.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (directory.get() < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        auto state =
            std::make_unique<State>(std::move(volume), std::move(staging), std::move(directory));
        state->faults = std::move(faults);
        state->manifest_limit = manifest_byte_limit;
        state->storage_bytes = seal.total_physical_bytes;
        if (fstat(state->directory.get(), &state->directory_identity) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        state->files.reserve(seal.files.size());
        state->evidence.reserve(seal.files.size());
        for (const auto& file : seal.files) {
            if (file.relative_path == "manifest.json" ||
                file.relative_path.size() > limits.max_relative_path_bytes) {
                return Status(StatusCode::InvalidArgument, {});
            }
            FileDescriptor fd(openat(
                state->directory.get(), file.relative_path.c_str(),
                O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
            struct stat attributes {};
            if (fd.get() < 0 || fstat(fd.get(), &attributes) != 0) {
                return Status(StatusCode::Unavailable, {});
            }
            auto digest = persistence::file_digest(fd.get(), file.physical_size);
            if (!digest.ok()) {
                return digest.status();
            }
            state->evidence.push_back(
                {file.relative_path, file.physical_size, std::move(digest.value())});
            state->files.emplace_back(std::move(fd), attributes);
        }
        auto result = std::unique_ptr<SealedFiles>(new SealedFiles(std::move(state)));
        valid = result->verify();
        if (!valid.ok()) {
            return valid.status();
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

const std::vector<FileEvidence>& SealedFiles::evidence() const noexcept { return state_->evidence; }

std::uint64_t SealedFiles::storage_bytes() const noexcept { return state_->storage_bytes; }

std::uint64_t SealedFiles::manifest_bytes() const noexcept { return state_->manifest_bytes; }

const std::filesystem::path& SealedFiles::staging_directory() const noexcept {
    return state_->staging;
}

Result<void> SealedFiles::verify() const {
    auto valid = state_->volume->verify();
    if (!valid.ok()) {
        return valid.status();
    }
    struct stat current {};
    if (lstat(state_->staging.c_str(), &current) != 0 || !S_ISDIR(current.st_mode) ||
        current.st_dev != state_->directory_identity.st_dev ||
        current.st_ino != state_->directory_identity.st_ino) {
        return Status(StatusCode::DataLoss, {});
    }
    for (std::size_t index = 0; index < state_->files.size(); ++index) {
        const auto& file = state_->files[index];
        if (fstat(file.descriptor.get(), &current) != 0 || !same_file(file.identity, current) ||
            fstatat(
                state_->directory.get(), state_->evidence[index].relative_path.c_str(), &current,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !same_file(file.identity, current)) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    if (state_->manifest &&
        (fstat(state_->manifest->descriptor.get(), &current) != 0 ||
         !same_file(state_->manifest->identity, current) ||
         fstatat(state_->directory.get(), "manifest.json", &current, AT_SYMLINK_NOFOLLOW) != 0 ||
         !same_file(state_->manifest->identity, current))) {
        return Status(StatusCode::DataLoss, {});
    }
    return state_->verify_entries();
}

Result<void> SealedFiles::install_manifest(std::string_view encoded_manifest) {
    if (encoded_manifest.empty() || encoded_manifest.size() > state_->manifest_limit ||
        state_->manifest_attempted) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto verified = verify();
    if (!verified.ok()) {
        return verified.status();
    }
    state_->manifest_attempted = true;
    auto publication = persistence::publish_file(
        state_->staging, "manifest.json", encoded_manifest, persistence::FilePublishMode::Create,
        static_cast<std::size_t>(state_->manifest_limit), state_->faults);
    if (!publication.status.ok()) {
        return std::move(publication.status);
    }
    try {
        FileDescriptor fd(openat(
            state_->directory.get(), "manifest.json",
            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        struct stat attributes {};
        if (fd.get() < 0 || fstat(fd.get(), &attributes) != 0 || !S_ISREG(attributes.st_mode) ||
            attributes.st_nlink != 1 || attributes.st_size < 0 ||
            static_cast<std::uint64_t>(attributes.st_size) != encoded_manifest.size()) {
            return Status(StatusCode::DataLoss, {});
        }
        state_->manifest = std::make_unique<OpenedFile>(std::move(fd), attributes);
        state_->manifest_bytes = encoded_manifest.size();
        return verify();
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> SealedFiles::sync_tree() {
    state_->synced = false;
    if (!state_->manifest) {
        return Status(StatusCode::InvalidState, {});
    }
    auto verified = verify();
    if (!verified.ok()) {
        return verified.status();
    }
    for (const auto& file : state_->files) {
        auto synced =
            synchronize(file.descriptor.get(), persistence::FileStep::SyncFile, state_->faults);
        if (!synced.ok()) {
            return synced.status();
        }
    }
    auto synced = synchronize(
        state_->manifest->descriptor.get(), persistence::FileStep::SyncFile, state_->faults);
    if (!synced.ok()) {
        return synced.status();
    }
    synced =
        synchronize(state_->directory.get(), persistence::FileStep::SyncDirectory, state_->faults);
    if (!synced.ok()) {
        return synced.status();
    }
    verified = verify();
    if (!verified.ok()) {
        return verified.status();
    }
    state_->synced = true;
    return {};
}

bool SealedFiles::durable_tree() const noexcept { return state_->synced; }

}  // namespace rodaq::artifact
