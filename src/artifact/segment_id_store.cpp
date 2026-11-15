#include "segment_id_store.hpp"

#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"

namespace rodaq::artifact {
namespace {

constexpr auto kFile = "segment-identity.json";
constexpr auto kLock = ".segment-identity.lock";
constexpr auto kDomain = "rodaq.segment-identity.v1";

persistence::JsonLimits limits() { return {1024, 2, 128, 3, 0, 4}; }

Result<std::uint64_t> decode(std::string_view bytes) {
    auto parsed = persistence::parse_json(bytes, limits());
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : parsed.status();
    }
    const auto& document = parsed.value();
    if (!document.is_object() || document.size() != 3) {
        return Status(StatusCode::DataLoss, {});
    }
    for (const auto* field : {"format_version", "reserved_end_exclusive", "content_digest"}) {
        if (!document.contains(field) || !document.at(field).is_string()) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    if (document.at("format_version") != "1.0") {
        return Status(StatusCode::Unavailable, {});
    }
    auto end = persistence::unsigned_decimal(
        document.at("reserved_end_exclusive").get_ref<const std::string&>());
    if (!end.ok() || end.value() == 0) {
        return Status(StatusCode::DataLoss, {});
    }
    auto verified = persistence::verify_integrity(document, kDomain, limits());
    if (!verified.ok()) {
        return verified.status();
    }
    return end.value();
}

Result<std::string> encode(std::uint64_t end) {
    try {
        return persistence::encode_integrity(
            {{"format_version", "1.0"}, {"reserved_end_exclusive", std::to_string(end)}}, kDomain,
            limits());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace

struct SegmentIdStore::State {
    ~State() noexcept {
        if (lock >= 0) {
            close(lock);
        }
        if (parent >= 0) {
            close(parent);
        }
    }

    std::filesystem::path directory;
    persistence::FileFaultHook faults;
    std::mutex execution;
    int parent{-1};
    int lock{-1};
    std::uint64_t end{1};
    std::uint64_t candidate_end{0};
    bool protected_state{false};
    std::string retained_temporary;
};

SegmentIdStore::SegmentIdStore(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

SegmentIdStore::~SegmentIdStore() noexcept = default;

Result<std::unique_ptr<SegmentIdStore>> SegmentIdStore::open(
    const std::filesystem::path& directory, SegmentIdOpenMode mode,
    persistence::FileFaultHook faults) {
    if (mode != SegmentIdOpenMode::Existing && mode != SegmentIdOpenMode::NewDeployment) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->directory = directory;
        state->faults = std::move(faults);
        state->parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (state->parent < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        state->lock = openat(
            state->parent, kLock, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
        if (state->lock < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        struct stat info {};
        if (fstat(state->lock, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
            return Status(StatusCode::DataLoss, {});
        }
        if (flock(state->lock, LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto current = persistence::read_file(directory, kFile, limits().max_bytes);
        if (current.ok()) {
            auto end = decode(current.value());
            if (!end.ok()) {
                return end.status();
            }
            state->end = end.value();
        } else if (
            current.status().code() == StatusCode::NotFound &&
            mode == SegmentIdOpenMode::NewDeployment) {
            auto bytes = encode(1);
            if (!bytes.ok()) {
                return bytes.status();
            }
            auto published = persistence::publish_file(
                directory, kFile, bytes.value(), persistence::FilePublishMode::Create,
                limits().max_bytes, state->faults);
            if (!published.status.ok()) {
                return std::move(published.status);
            }
        } else {
            return current.status().code() == StatusCode::NotFound
                       ? Status(StatusCode::DataLoss, {})
                       : current.status();
        }
        // Exclusive namespace ownership makes these recognizable unpublished
        // atomic-file remnants safe to clean. Never infer IDs from their content.
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (++count > 64) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            const auto name = entry.path().filename().string();
            if (name == kFile || name == kLock) {
                continue;
            }
            if (!persistence::is_temporary_file_name(name) || entry.is_symlink() ||
                !entry.is_regular_file()) {
                return Status(StatusCode::DataLoss, {});
            }
            std::filesystem::remove(entry.path());
        }
        auto synced = persistence::sync_file(directory, kFile, state->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        synced = persistence::sync_directory(directory, state->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        return std::unique_ptr<SegmentIdStore>(new SegmentIdStore(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<SegmentIdRange> SegmentIdStore::reserve(std::uint32_t count) {
    if (count == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    std::unique_lock<std::mutex> lock(state_->execution, std::try_to_lock);
    if (!lock.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    if (state_->protected_state) {
        return Status(StatusCode::Unavailable, {});
    }
    if (count > std::numeric_limits<std::uint64_t>::max() - state_->end) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    auto current = persistence::read_file(state_->directory, kFile, limits().max_bytes);
    if (!current.ok()) {
        state_->protected_state = true;
        return current.status();
    }
    auto actual = decode(current.value());
    if (!actual.ok() || actual.value() != state_->end) {
        state_->protected_state = true;
        return actual.ok() ? Status(StatusCode::DataLoss, {}) : actual.status();
    }
    auto bytes = encode(state_->end + count);
    if (!bytes.ok()) {
        return bytes.status();
    }
    state_->candidate_end = state_->end + count;
    auto published = persistence::publish_file(
        state_->directory, kFile, bytes.value(), persistence::FilePublishMode::Replace,
        limits().max_bytes, state_->faults);
    if (!published.status.ok()) {
        state_->protected_state = true;
        state_->retained_temporary = std::move(published.retained_temporary);
        return std::move(published.status);
    }
    const SegmentIdRange range{state_->end, state_->candidate_end};
    state_->end = state_->candidate_end;
    state_->candidate_end = 0;
    return range;
}

Result<void> SegmentIdStore::reconcile() {
    std::unique_lock<std::mutex> lock(state_->execution, std::try_to_lock);
    if (!lock.owns_lock()) {
        return Status(StatusCode::Conflict, {});
    }
    auto current = persistence::read_file(state_->directory, kFile, limits().max_bytes);
    if (!current.ok()) {
        return current.status();
    }
    auto actual = decode(current.value());
    if (!actual.ok()) {
        return actual.status();
    }
    if (actual.value() != state_->end && actual.value() != state_->candidate_end) {
        return Status(StatusCode::DataLoss, {});
    }
    auto synced = persistence::sync_file(state_->directory, kFile, state_->faults);
    if (!synced.ok()) {
        return synced.status();
    }
    if (!state_->retained_temporary.empty()) {
        if (unlinkat(state_->parent, state_->retained_temporary.c_str(), 0) != 0 &&
            errno != ENOENT) {
            return Status(StatusCode::Unavailable, {});
        }
    }
    synced = persistence::sync_directory(state_->directory, state_->faults);
    if (!synced.ok()) {
        return synced.status();
    }
    state_->end = actual.value();
    state_->candidate_end = 0;
    state_->retained_temporary.clear();
    state_->protected_state = false;
    return {};
}

}  // namespace rodaq::artifact
