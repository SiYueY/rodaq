#include "playback_anchor_store.hpp"

#include "../persistence/decimal.hpp"
#include "../persistence/json.hpp"

#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace rodaq::time {
namespace {

constexpr std::string_view kFile = "playback-anchor.json";
constexpr std::string_view kDomain = "rodaq.playback-anchor.v1";

persistence::JsonLimits limits() { return {4096, 4, 128, 8, 0, 16}; }

struct PersistedTime {
    BootIdentity boot;
    PlaybackAnchor anchor;
    std::uint64_t reserved_epoch_end{0};
    std::string digest;
};

Result<PersistedTime> decode(std::string_view bytes) {
    auto parsed = persistence::parse_json(bytes, limits());
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : parsed.status();
    }
    const auto& document = parsed.value();
    if (!document.is_object() || document.size() != 7) {
        return Status(StatusCode::DataLoss, {});
    }
    for (const auto* field :
         {"format_version", "system_boot_id", "kernel_boot_uuid", "playback_anchor_monotonic",
          "playback_anchor_realtime", "reserved_epoch_end_exclusive", "content_digest"}) {
        const auto entry = document.find(field);
        if (entry == document.end() || !entry->is_string()) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    const auto text = [&](const char* field) -> const std::string& {
        return document.at(field).get_ref<const std::string&>();
    };
    if (text("format_version") != "1.0") {
        return Status(StatusCode::Unavailable, {});
    }
    if (text("kernel_boot_uuid").size() != 36) {
        return Status(StatusCode::DataLoss, {});
    }
    auto boot = parse_boot_identity(text("kernel_boot_uuid"));
    if (!boot.ok()) {
        return boot.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : boot.status();
    }
    auto identity = persistence::unsigned_decimal(text("system_boot_id"));
    auto monotonic = persistence::unsigned_decimal(text("playback_anchor_monotonic"));
    auto realtime = persistence::signed_decimal(text("playback_anchor_realtime"));
    auto end = persistence::unsigned_decimal(text("reserved_epoch_end_exclusive"));
    if (!boot.ok() || !identity.ok() || !monotonic.ok() || !realtime.ok() || !end.ok() ||
        identity.value() != boot.value().id || end.value() < 2) {
        return Status(StatusCode::DataLoss, {});
    }
    // Schema/strong-type validation precedes semantic integrity verification.
    auto verified = persistence::verify_integrity(document, kDomain, limits());
    if (!verified.ok()) {
        return verified.status();
    }
    return PersistedTime{
        std::move(boot).value(),
        {identity.value(), monotonic.value(), realtime.value()},
        end.value(),
        text("content_digest")};
}

Result<void> clean_unpublished_temporaries(const std::filesystem::path& directory) {
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (++entries > 8) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        const auto name = entry.path().filename().string();
        if (name == kFile || name == ".clock.lock") {
            continue;
        }
        // With the exclusive writer lock, these can only be unpublished work
        // from an earlier failed/crashed owner. They never establish authority.
        if (name.size() != 43 || name.compare(0, 11, ".rodaq-tmp-") != 0 ||
            !std::all_of(
                name.begin() + 11, name.end(),
                [](char value) {
                    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                }) ||
            !entry.is_regular_file() || entry.is_symlink()) {
            return Status(StatusCode::DataLoss, {});
        }
        std::filesystem::remove(entry.path());
    }
    // The following record publish fsyncs this same parent before any epoch use.
    return {};
}

}  // namespace

struct PlaybackAnchorStore::State {
    ~State() noexcept {
        if (lock >= 0) {
            close(lock);
        }
        if (parent >= 0) {
            close(parent);
        }
    }

    std::filesystem::path directory;
    BootIdentity boot;
    PlaybackAnchor anchor;
    EpochRange initial;
    std::uint64_t reserved_epoch_end{1};
    std::uint32_t reservation_count{0};
    bool record_exists{false};
    bool failed{false};
    int parent{-1};
    int lock{-1};
    std::string retained_temporary;
    std::string expected_digest;
    persistence::FileFaultHook faults;
    // This serializes durable execution only. Snapshot readers never take it.
    std::mutex publication;
};

PlaybackAnchorStore::PlaybackAnchorStore(std::unique_ptr<State> state) noexcept
: state_(std::move(state)) {}

PlaybackAnchorStore::~PlaybackAnchorStore() noexcept = default;

Result<std::unique_ptr<PlaybackAnchorStore>> PlaybackAnchorStore::open(
    const std::filesystem::path& directory, const BootIdentity& boot,
    const ClockSample& initial_sample, BootArtifactEvidence artifacts,
    std::uint32_t epoch_reservation_count, persistence::FileFaultHook faults) {
    auto verified_boot = parse_boot_identity(boot.kernel_uuid);
    if (!verified_boot.ok()) {
        return verified_boot.status();
    }
    if (verified_boot.value().id != boot.id ||
        verified_boot.value().kernel_uuid != boot.kernel_uuid ||
        (artifacts.state != BootArtifactState::Absent &&
         artifacts.state != BootArtifactState::Present &&
         artifacts.state != BootArtifactState::Unknown) ||
        (artifacts.state == BootArtifactState::Present && artifacts.anchor.boot_id != boot.id) ||
        epoch_reservation_count == 0 || epoch_reservation_count > 65536) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->directory = directory;
        state->boot = boot;
        state->anchor = {boot.id, initial_sample.monotonic_ns, initial_sample.realtime_ns};
        state->reservation_count = epoch_reservation_count;
        state->faults = std::move(faults);
        state->parent = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (state->parent < 0) {
            return Status(StatusCode::Unavailable, {});
        }
        state->lock = openat(
            state->parent, ".clock.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
            0600);
        if (state->lock < 0) {
            return Status(
                errno == EACCES || errno == EROFS ? StatusCode::PermissionDenied
                                                  : StatusCode::Unavailable,
                {});
        }
        struct stat lock_status {};
        if (fstat(state->lock, &lock_status) != 0 || !S_ISREG(lock_status.st_mode)) {
            return Status(StatusCode::DataLoss, {});
        }
        if (flock(state->lock, LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        auto existing = persistence::read_file(directory, kFile, limits().max_bytes);
        bool current_anchor = false;
        if (existing.ok()) {
            auto stored = decode(existing.value());
            if (!stored.ok()) {
                return stored.status();
            }
            state->record_exists = true;
            state->expected_digest = stored.value().digest;
            if (stored.value().boot.kernel_uuid == boot.kernel_uuid) {
                current_anchor = true;
                state->anchor = stored.value().anchor;
                state->reserved_epoch_end = stored.value().reserved_epoch_end;
                if (initial_sample.monotonic_ns < state->anchor.monotonic_ns) {
                    return Status(StatusCode::DataLoss, {});
                }
            } else if (stored.value().boot.id == boot.id) {
                return Status(StatusCode::DataLoss, {});  // 64-bit identity collision; never guess.
            }
        } else if (existing.status().code() != StatusCode::NotFound) {
            return existing.status().code() == StatusCode::InvalidArgument
                       ? Status(StatusCode::DataLoss, {})
                       : existing.status();
        }
        if (current_anchor && artifacts.state == BootArtifactState::Present &&
            (artifacts.anchor.monotonic_ns != state->anchor.monotonic_ns ||
             artifacts.anchor.realtime_ns != state->anchor.realtime_ns)) {
            return Status(StatusCode::DataLoss, {});
        }
        if (!current_anchor && artifacts.state != BootArtifactState::Absent) {
            return Status(
                artifacts.state == BootArtifactState::Present ? StatusCode::DataLoss
                                                              : StatusCode::Unavailable,
                {});
        }
        auto cleaned = clean_unpublished_temporaries(directory);
        if (!cleaned.ok()) {
            return cleaned.status();
        }
        auto owner =
            std::unique_ptr<PlaybackAnchorStore>(new PlaybackAnchorStore(std::move(state)));
        auto reserved = owner->reserve_epochs();
        if (!reserved.ok()) {
            return reserved.status();
        }
        owner->state_->initial = reserved.value();
        return owner;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    } catch (const persistence::Json::exception&) {
        return Status(StatusCode::DataLoss, {});
    }
}

PlaybackAnchor PlaybackAnchorStore::anchor() const noexcept { return state_->anchor; }

EpochRange PlaybackAnchorStore::initial_range() const noexcept { return state_->initial; }

Result<EpochRange> PlaybackAnchorStore::reserve_epochs() {
    std::lock_guard<std::mutex> execution(state_->publication);
    if (state_->failed) {
        return Status(StatusCode::InvalidState, {});
    }
    const auto begin = state_->reserved_epoch_end;
    if (state_->reservation_count > std::numeric_limits<std::uint64_t>::max() - begin) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    try {
        struct stat pinned {};
        struct stat named {};
        if (fstat(state_->parent, &pinned) != 0 || stat(state_->directory.c_str(), &named) != 0 ||
            pinned.st_dev != named.st_dev || pinned.st_ino != named.st_ino) {
            state_->failed = true;
            return Status(StatusCode::Unavailable, {});
        }
        const auto end = begin + state_->reservation_count;
        if (state_->record_exists) {
            auto actual = persistence::read_file(state_->directory, kFile, limits().max_bytes);
            if (!actual.ok()) {
                state_->failed = true;
                return actual.status();
            }
            auto record = decode(actual.value());
            if (!record.ok() || record.value().digest != state_->expected_digest) {
                state_->failed = true;
                return record.ok() ? Status(StatusCode::DataLoss, {}) : record.status();
            }
        }
        const persistence::Json document{
            {"format_version", "1.0"},
            {"system_boot_id", std::to_string(state_->boot.id)},
            {"kernel_boot_uuid", state_->boot.kernel_uuid},
            {"playback_anchor_monotonic", std::to_string(state_->anchor.monotonic_ns)},
            {"playback_anchor_realtime", std::to_string(state_->anchor.realtime_ns)},
            {"reserved_epoch_end_exclusive", std::to_string(end)}};
        auto encoded = persistence::encode_integrity(document, kDomain, limits());
        if (!encoded.ok()) {
            return encoded.status();
        }
        auto next = decode(encoded.value());
        if (!next.ok()) {
            return next.status();
        }
        auto published = persistence::publish_file(
            state_->directory, kFile, encoded.value(),
            state_->record_exists ? persistence::FilePublishMode::Replace
                                  : persistence::FilePublishMode::Create,
            limits().max_bytes, state_->faults);
        if (!published.status.ok() ||
            published.disposition != persistence::FileDisposition::Durable) {
            state_->failed = true;
            state_->retained_temporary = std::move(published.retained_temporary);
            return published.status.ok() ? Status(StatusCode::Internal, {}) : published.status;
        }
        state_->record_exists = true;
        state_->reserved_epoch_end = end;
        state_->expected_digest = std::move(next).value().digest;
        return EpochRange{begin, end};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::time
