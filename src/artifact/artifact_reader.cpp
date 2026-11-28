#include <rodaq/artifact/artifact_service.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "catalog_state.hpp"

namespace rodaq::artifact {

ReadBacking::~ReadBacking() noexcept {
    // An actual read owns a shared reference until its body returns. Close the
    // fd before retiring its lease, never while holding the Catalog index lock.
    file.reset();
    if (!registered) {
        return;
    }
    std::lock_guard<std::mutex> lock(owner->index);
    auto& borrowed = owner->readers[slot];
    if (borrowed.busy && borrowed.generation == generation) {
        borrowed.busy = false;
        borrowed.reader.reset();
        --entry->readers;
        --owner->read_leases;
        owner->reader_changed.notify_all();
    } else {
        owner->accepting_reads = false;
    }
}

Result<ArtifactReadHandle> ArtifactCatalog::acquire(ArtifactId id, ArtifactFileId file) {
    if (id.value == 0 || file.value == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto state = state_;
    try {
        auto backing = std::make_shared<ReadBacking>();
        backing->owner = state;
        const std::size_t file_index = static_cast<std::size_t>(file.value - 1);
        {
            std::lock_guard<std::mutex> lock(state->index);
            if (!state->accepting_reads) {
                return Status(StatusCode::InvalidState, {});
            }
            if (state->options.volume->protection() != StatusCode::Ok) {
                return Status(state->options.volume->protection(), {});
            }
            const auto at = state->find(id.value);
            if (at == state->slots.size() || !state->slots[at].visible || !state->slots[at].entry) {
                return Status(StatusCode::NotFound, {});
            }
            auto entry = state->slots[at].entry;
            if (entry->read_blocked) {
                return Status(StatusCode::InvalidState, {});
            }
            if (file_index >= entry->files.size()) {
                return Status(StatusCode::NotFound, {});
            }
            const auto available =
                std::find_if(state->readers.begin(), state->readers.end(), [](const auto& slot) {
                    return !slot.busy &&
                           slot.generation != std::numeric_limits<std::uint64_t>::max();
                });
            if (available == state->readers.end()) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            backing->entry = std::move(entry);
            backing->slot = static_cast<std::size_t>(available - state->readers.begin());
            backing->generation = ++available->generation;
            backing->length = backing->entry->info.files[file_index].size;
            backing->deadline =
                std::chrono::steady_clock::now() + state->options.read_lease_duration;
            available->reader = backing;
            available->busy = true;
            backing->registered = true;
            ++backing->entry->readers;
            ++state->read_leases;
        }
        if (state->options.read_faults && state->options.read_faults(ReadStep::Opening) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        auto directory = open_final_directory(state->options.volume, id.value);
        if (!directory.ok()) {
            if (directory.status().code() == StatusCode::DataLoss) {
                state->corrupt(id.value);
            }
            return directory.status();
        }
        struct stat actual_directory {};
        struct stat actual_manifest {};
        const auto& expected = backing->entry->files[file_index];
        if (fstat(directory.value().get(), &actual_directory) != 0 ||
            !same_segment_directory(backing->entry->directory, actual_directory) ||
            fstatat(
                directory.value().get(), "manifest.json", &actual_manifest, AT_SYMLINK_NOFOLLOW) !=
                0 ||
            !same_regular_file(backing->entry->manifest, actual_manifest)) {
            state->corrupt(id.value);
            return Status(StatusCode::DataLoss, {});
        }
        const int opened = openat(
            directory.value().get(), expected.path.c_str(),
            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (opened < 0) {
            if (errno == ENOENT || errno == ELOOP || errno == ENOTDIR) {
                state->corrupt(id.value);
                return Status(StatusCode::DataLoss, {});
            }
            return Status(StatusCode::Unavailable, {});
        }
        backing->file.emplace(opened);
        if (fstat(opened, &backing->identity) != 0 ||
            !same_regular_file(expected.identity, backing->identity)) {
            state->corrupt(id.value);
            return Status(StatusCode::DataLoss, {});
        }
        if (backing->cancelled.load()) {
            return Status(StatusCode::InvalidState, {});
        }
        return ArtifactReadHandle(std::move(backing));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

Result<void> ArtifactCatalog::block_reads(ArtifactId id) {
    auto state = state_;
    std::lock_guard<std::mutex> lock(state->index);
    const auto at = state->find(id.value);
    if (at == state->slots.size() || !state->slots[at].entry) {
        return Status(StatusCode::NotFound, {});
    }
    if (state->slots[at].work != CatalogWork::None) {
        return Status(StatusCode::Conflict, {});
    }
    state->slots[at].entry->read_blocked = true;
    return {};
}

void ArtifactCatalog::cancel_readers(std::optional<ArtifactId> id) noexcept {
    auto state = state_;
    std::lock_guard<std::mutex> cancel_lock(state->cancel_mutex);
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(state->index);
        if (!id) {
            state->accepting_reads = false;
        }
        for (const auto& slot : state->readers) {
            auto reader = slot.reader.lock();
            if (reader && count < state->cancel_scratch.size()) {
                state->cancel_scratch[count++] = std::move(reader);
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& reader = state->cancel_scratch[i];
        if (!id || reader->entry->info.summary.artifact_id == *id) {
            reader->cancelled.store(true);
        }
        reader.reset();
    }
}

Result<void> ArtifactCatalog::wait_readers(
    ArtifactId id, std::chrono::steady_clock::time_point deadline) {
    auto state = state_;
    std::unique_lock<std::mutex> lock(state->index);
    const auto at = state->find(id.value);
    if (at == state->slots.size() || !state->slots[at].entry) {
        return Status(StatusCode::NotFound, {});
    }
    if (!state->reader_changed.wait_until(
            lock, deadline, [&] { return state->slots[at].entry->readers == 0; })) {
        return Status(StatusCode::DeadlineExceeded, {});
    }
    return {};
}

}  // namespace rodaq::artifact

namespace rodaq {

ArtifactReadHandle::ArtifactReadHandle(std::shared_ptr<artifact::ReadBacking> backing) noexcept
: backing_(std::move(backing)) {}

ArtifactReadHandle::ArtifactReadHandle(ArtifactReadHandle&& other) noexcept
: backing_(std::move(other.backing_)) {}

ArtifactReadHandle& ArtifactReadHandle::operator=(ArtifactReadHandle&& other) noexcept {
    if (this != &other) {
        request_cancel();
        backing_ = std::move(other.backing_);
    }
    return *this;
}

ArtifactReadHandle::~ArtifactReadHandle() noexcept { request_cancel(); }

std::uint64_t ArtifactReadHandle::size() const noexcept { return backing_ ? backing_->length : 0; }

void ArtifactReadHandle::request_cancel() noexcept {
    if (backing_) {
        backing_->cancelled.store(true);
    }
}

Result<std::size_t> ArtifactReadHandle::read_at(std::uint64_t offset, MutableBytes output) {
    // The wrapper may be released after actual entry (e.g. disconnect). No
    // access to the wrapper follows this copy; fd and lease self-retain below.
    auto backing = backing_;
    if (!backing || backing->cancelled.load() ||
        std::chrono::steady_clock::now() >= backing->deadline) {
        return Status(StatusCode::InvalidState, {});
    }
    if ((!output.data && output.size != 0) ||
        output.size > backing->owner->options.max_read_bytes || offset > backing->length ||
        offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return Status(StatusCode::InvalidArgument, {});
    }
    bool idle = false;
    if (!backing->active.compare_exchange_strong(idle, true)) {
        return Status(StatusCode::Conflict, {});
    }
    backing->owner->active_reads.fetch_add(1);
    struct ActualRead {
        std::shared_ptr<artifact::ReadBacking> backing;
        ~ActualRead() {
            backing->owner->active_reads.fetch_sub(1);
            backing->active.store(false);
        }
    } actual{backing};
    try {
        if (backing->owner->options.read_faults &&
            backing->owner->options.read_faults(artifact::ReadStep::Reading) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        auto volume = backing->owner->options.volume->verify();
        if (!volume.ok()) {
            return volume.status();
        }
        struct stat before {};
        if (!backing->file || fstat(backing->file->get(), &before) != 0 ||
            !artifact::same_regular_file(backing->identity, before)) {
            backing->owner->corrupt(backing->entry->info.summary.segment_id);
            return Status(StatusCode::DataLoss, {});
        }
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(output.size, backing->length - offset));
        ssize_t received = 0;
        if (count != 0) {
            do {
                received =
                    pread(backing->file->get(), output.data, count, static_cast<off_t>(offset));
            } while (received < 0 && errno == EINTR);
        }
        if (received < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        struct stat after {};
        if ((received == 0 && count != 0) || fstat(backing->file->get(), &after) != 0 ||
            !artifact::same_regular_file(backing->identity, after)) {
            backing->owner->corrupt(backing->entry->info.summary.segment_id);
            return Status(StatusCode::DataLoss, {});
        }
        if (backing->cancelled.load()) {
            return Status(StatusCode::InvalidState, {});
        }
        return static_cast<std::size_t>(received);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

}  // namespace rodaq
