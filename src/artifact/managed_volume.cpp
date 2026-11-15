#include "managed_volume.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <new>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

#include "../persistence/json.hpp"
#include "../time/clock.hpp"
#include "../time/wall_time.hpp"
#include "../uuid.hpp"

namespace rodaq::artifact {
namespace {

constexpr auto kDomain = "rodaq.volume.v1";
persistence::JsonLimits limits() { return {4096, 2, 128, 4, 0, 5}; }

class FileDescriptor {
public:
    explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    ~FileDescriptor() noexcept {
        if (value_ >= 0) {
            close(value_);
        }
    }

    int get() const noexcept { return value_; }

private:
    int value_;
};

Status filesystem_failure() {
    return Status(
        errno == EACCES || errno == EROFS ? StatusCode::PermissionDenied : StatusCode::Unavailable,
        {});
}

Result<std::string> unescape_mount(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t offset = 0; offset < input.size(); ++offset) {
        if (input[offset] != '\\') {
            result += input[offset];
            continue;
        }
        if (offset + 3 >= input.size()) {
            return Status(StatusCode::DataLoss, {});
        }
        const auto escape = input.substr(offset + 1, 3);
        if (escape == "040") {
            result += ' ';
        } else if (escape == "011") {
            result += '\t';
        } else if (escape == "012") {
            result += '\n';
        } else if (escape == "134") {
            result += '\\';
        } else {
            return Status(StatusCode::DataLoss, {});
        }
        offset += 3;
    }
    return result;
}

std::string_view take_field(std::string_view& line) noexcept {
    const auto end = line.find(' ');
    auto field = line.substr(0, end);
    line = end == std::string_view::npos ? std::string_view{} : line.substr(end + 1);
    return field;
}

Result<VolumeRuntimeIdentity> runtime_identity(int directory) {
    struct stat attributes {};
    struct statx extended {};
    if (fstat(directory, &attributes) != 0 ||
        statx(directory, "", AT_EMPTY_PATH | AT_STATX_SYNC_AS_STAT, STATX_MNT_ID, &extended) != 0) {
        return filesystem_failure();
    }
    if ((extended.stx_mask & STATX_MNT_ID) == 0 || extended.stx_mnt_id == 0) {
        return Status(StatusCode::Unavailable, {});
    }
    std::ifstream mounts("/proc/self/mountinfo");
    if (!mounts) {
        return Status(StatusCode::Unavailable, {});
    }
    std::array<char, 32768> buffer{};
    std::size_t total = 0;
    for (std::size_t line_count = 0; line_count < 16384; ++line_count) {
        mounts.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (mounts.fail()) {
            return Status(
                mounts.eof() ? StatusCode::Unavailable : StatusCode::ResourceExhausted, {});
        }
        const std::string_view line(buffer.data());
        total += line.size();
        if (total > 8 * 1024 * 1024) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        auto fields = line;
        const auto identity = take_field(fields);
        std::uint64_t mount_id = 0;
        const auto parsed =
            std::from_chars(identity.data(), identity.data() + identity.size(), mount_id);
        if (parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size() ||
            mount_id != extended.stx_mnt_id) {
            continue;
        }
        (void)take_field(fields);  // parent ID
        (void)take_field(fields);  // major:minor; fstat supplies exact st_dev
        (void)take_field(fields);  // mounted filesystem's root
        const auto point = take_field(fields);
        const auto options = take_field(fields);
        const auto separator = fields.find(" - ");
        if (fields.substr(0, 2) == "- ") {
            fields.remove_prefix(2);
        } else if (separator != std::string_view::npos) {
            fields.remove_prefix(separator + 3);
        } else {
            return Status(StatusCode::DataLoss, {});
        }
        const auto type = take_field(fields);
        (void)take_field(fields);  // mount source
        const auto super = take_field(fields);
        if (point.empty() || type.empty() || options.empty() || super.empty()) {
            return Status(StatusCode::DataLoss, {});
        }
        auto decoded = unescape_mount(point);
        if (!decoded.ok()) {
            return decoded.status();
        }
        return VolumeRuntimeIdentity{
            static_cast<std::uint64_t>(attributes.st_dev),
            static_cast<std::uint64_t>(attributes.st_ino),
            mount_id,
            std::string(type),
            std::move(decoded.value()),
            std::string(options),
            std::string(super)};
    }
    return Status(StatusCode::ResourceExhausted, {});
}

struct Marker {
    VolumeId id;
    std::string digest;
};

Result<Marker> decode_marker(std::string_view bytes) {
    auto parsed = persistence::parse_json(bytes, limits());
    if (!parsed.ok()) {
        return parsed.status().code() == StatusCode::InvalidArgument
                   ? Status(StatusCode::DataLoss, {})
                   : parsed.status();
    }
    const auto& document = parsed.value();
    if (!document.is_object() || document.size() != 4) {
        return Status(StatusCode::DataLoss, {});
    }
    for (const auto* field : {"format_version", "volume_id", "created_at", "content_digest"}) {
        if (!document.contains(field) || !document.at(field).is_string()) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    if (document.at("format_version") != "1.0") {
        return Status(StatusCode::Unavailable, {});
    }
    const auto text = [&](const char* key) -> const std::string& {
        return document.at(key).get_ref<const std::string&>();
    };
    auto identity = parse_volume_id(text("volume_id"));
    auto created = time::parse_wall_time(text("created_at"));
    if (!identity.ok() || !created.ok()) {
        return Status(StatusCode::DataLoss, {});
    }
    auto verified = persistence::verify_integrity(document, kDomain, limits());
    if (!verified.ok()) {
        return verified.status();
    }
    return Marker{identity.value(), text("content_digest")};
}

Result<void> prepare_directory(
    const std::filesystem::path& parent, const char* name,
    const persistence::FileFaultHook& faults) {
    FileDescriptor directory(
        ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0) {
        return filesystem_failure();
    }
    if (mkdirat(directory.get(), name, 0700) != 0 && errno != EEXIST) {
        return filesystem_failure();
    }
    FileDescriptor child(
        openat(directory.get(), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (child.get() < 0) {
        return filesystem_failure();
    }
    auto synced = persistence::sync_directory(parent / name, faults);
    if (!synced.ok()) {
        return synced.status();
    }
    return persistence::sync_directory(parent, faults);
}

bool same_runtime(const VolumeRuntimeIdentity& left, const VolumeRuntimeIdentity& right) noexcept {
    return left.device == right.device && left.inode == right.inode &&
           left.mount_id == right.mount_id && left.filesystem_type == right.filesystem_type &&
           left.mount_point == right.mount_point && left.mount_options == right.mount_options &&
           left.super_options == right.super_options;
}

Result<struct statx> directory_identity(const std::filesystem::path& path) {
    struct statx result {};
    constexpr unsigned kMask = STATX_MNT_ID | STATX_INO | STATX_TYPE;
    if (statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, kMask, &result) != 0) {
        return filesystem_failure();
    }
    if ((result.stx_mask & kMask) != kMask || !S_ISDIR(result.stx_mode)) {
        return Status(StatusCode::DataLoss, {});
    }
    return result;
}

}  // namespace

struct ManagedVolume::State {
    ~State() noexcept {
        if (lease >= 0) {
            close(lease);
        }
        if (root_fd >= 0) {
            close(root_fd);
        }
    }

    std::filesystem::path configured_root;
    std::filesystem::path root;
    std::filesystem::path metadata;
    std::filesystem::path artifacts;
    VolumeRuntimeIdentity runtime;
    std::array<struct statx, 3> managed_directories{};
    Marker marker;
    persistence::FileFaultHook faults;
    int root_fd{-1};
    int lease{-1};
    struct stat lease_identity {};
    mutable std::atomic<StatusCode> protection{StatusCode::Ok};

    Status protect(StatusCode code) const {
        auto expected = StatusCode::Ok;
        protection.compare_exchange_strong(expected, code);
        return Status(code, {});
    }
};

Result<VolumeId> parse_volume_id(std::string_view text) {
    auto parsed = uuid::parse(text);
    if (!parsed.ok()) {
        return parsed.status();
    }
    return VolumeId{parsed.value()};
}

std::string to_string(VolumeId id) { return uuid::format(id.value); }

ManagedVolume::ManagedVolume(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

ManagedVolume::~ManagedVolume() noexcept = default;

Result<std::shared_ptr<ManagedVolume>> ManagedVolume::open(
    const std::filesystem::path& root, VolumeOpenMode mode, VolumeId expected_identity,
    persistence::FileFaultHook faults) {
    if ((mode != VolumeOpenMode::Initialize && mode != VolumeOpenMode::Existing) ||
        (mode == VolumeOpenMode::Existing && !uuid::nonzero(expected_identity.value))) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto state = std::make_unique<State>();
        state->configured_root = std::filesystem::absolute(root);
        state->root = std::filesystem::canonical(root);
        state->metadata = state->root / ".rodaq";
        state->artifacts = state->metadata / "artifacts";
        state->faults = std::move(faults);
        state->root_fd =
            ::open(state->root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (state->root_fd < 0) {
            return filesystem_failure();
        }
        auto runtime = artifact::runtime_identity(state->root_fd);
        if (!runtime.ok()) {
            return runtime.status();
        }
        state->runtime = std::move(runtime.value());
        if (mode == VolumeOpenMode::Initialize) {
            auto prepared = prepare_directory(state->root, ".rodaq", state->faults);
            if (!prepared.ok()) {
                return prepared.status();
            }
        }
        FileDescriptor metadata(
            ::open(state->metadata.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (metadata.get() < 0) {
            return Status(StatusCode::DataLoss, {});
        }
        state->lease = openat(
            metadata.get(), ".volume.lock", O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC,
            0600);
        struct stat info {};
        if (state->lease < 0 || fstat(state->lease, &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_nlink != 1) {
            return Status(StatusCode::DataLoss, {});
        }
        if (flock(state->lease, LOCK_EX | LOCK_NB) != 0) {
            return Status(
                errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
        }
        state->lease_identity = info;
        auto bytes = persistence::read_file(state->metadata, "volume.json", limits().max_bytes);
        if (!bytes.ok() && bytes.status().code() == StatusCode::NotFound &&
            mode == VolumeOpenMode::Initialize) {
            if (uuid::nonzero(expected_identity.value)) {
                return Status(StatusCode::DataLoss, {});
            }
            // Never create a new identity over existing artifacts or other facts.
            std::size_t entries = 0;
            for (const auto& entry : std::filesystem::directory_iterator(state->metadata)) {
                if (++entries > 1 || entry.path().filename() != ".volume.lock") {
                    return Status(StatusCode::DataLoss, {});
                }
            }
            auto random = uuid::generate();
            auto now = time::sample_clocks();
            if (!random.ok()) {
                return random.status();
            }
            if (!now.ok()) {
                return now.status();
            }
            auto created = time::format_wall_time(
                time::WallTime(std::chrono::nanoseconds(now.value().realtime_ns)));
            if (!created.ok()) {
                return created.status();
            }
            bytes = persistence::encode_integrity(
                {{"format_version", "1.0"},
                 {"volume_id", uuid::format(random.value())},
                 {"created_at", created.value()}},
                kDomain, limits());
            if (!bytes.ok()) {
                return bytes.status();
            }
            auto published = persistence::publish_file(
                state->metadata, "volume.json", bytes.value(), persistence::FilePublishMode::Create,
                limits().max_bytes, state->faults);
            if (!published.status.ok()) {
                return std::move(published.status);
            }
        }
        if (!bytes.ok()) {
            return bytes.status().code() == StatusCode::NotFound ? Status(StatusCode::DataLoss, {})
                                                                 : bytes.status();
        }
        auto marker = decode_marker(bytes.value());
        if (!marker.ok()) {
            return marker.status();
        }
        if (uuid::nonzero(expected_identity.value) && !(expected_identity == marker.value().id)) {
            return Status(StatusCode::Conflict, {});
        }
        state->marker = std::move(marker.value());
        auto synced = persistence::sync_file(state->metadata, "volume.json", state->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        synced = persistence::sync_directory(state->metadata, state->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        synced = persistence::sync_directory(state->root, state->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        for (const auto* name : {"artifacts", "quarantine"}) {
            synced = prepare_directory(state->metadata, name, state->faults);
            if (!synced.ok()) {
                return synced.status();
            }
        }
        std::size_t index = 0;
        for (const auto& path :
             {state->metadata, state->artifacts, state->metadata / "quarantine"}) {
            auto captured = directory_identity(path);
            if (!captured.ok()) {
                return captured.status();
            }
            state->managed_directories[index++] = captured.value();
        }
        auto result = std::shared_ptr<ManagedVolume>(new ManagedVolume(std::move(state)));
        auto verified = result->verify();
        if (!verified.ok()) {
            return verified.status();
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

VolumeId ManagedVolume::identity() const noexcept { return state_->marker.id; }

const VolumeRuntimeIdentity& ManagedVolume::runtime_identity() const noexcept {
    return state_->runtime;
}

const std::filesystem::path& ManagedVolume::root() const noexcept { return state_->root; }

const std::filesystem::path& ManagedVolume::artifacts_directory() const noexcept {
    return state_->artifacts;
}

StatusCode ManagedVolume::protection() const noexcept { return state_->protection.load(); }

Result<void> ManagedVolume::verify() const {
    if (protection() != StatusCode::Ok) {
        return Status(protection(), {});
    }
    try {
        if (std::filesystem::canonical(state_->configured_root) != state_->root) {
            return state_->protect(StatusCode::DataLoss);
        }
        FileDescriptor current(
            ::open(state_->root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (current.get() < 0) {
            return state_->protect(StatusCode::Unavailable);
        }
        auto runtime = artifact::runtime_identity(current.get());
        if (!runtime.ok()) {
            return state_->protect(runtime.status().code());
        }
        if (!same_runtime(state_->runtime, runtime.value())) {
            return state_->protect(StatusCode::DataLoss);
        }
        std::size_t index = 0;
        for (const auto& path :
             {state_->metadata, state_->artifacts, state_->metadata / "quarantine"}) {
            auto actual = directory_identity(path);
            if (!actual.ok()) {
                return state_->protect(actual.status().code());
            }
            const auto& expected = state_->managed_directories[index++];
            if (actual.value().stx_ino != expected.stx_ino ||
                actual.value().stx_mnt_id != expected.stx_mnt_id) {
                return state_->protect(StatusCode::DataLoss);
            }
        }
        struct stat lease {};
        if (fstatat(
                AT_FDCWD, (state_->metadata / ".volume.lock").c_str(), &lease,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(lease.st_mode) || lease.st_nlink != 1 ||
            lease.st_dev != state_->lease_identity.st_dev ||
            lease.st_ino != state_->lease_identity.st_ino) {
            return state_->protect(StatusCode::DataLoss);
        }
        auto bytes = persistence::read_file(state_->metadata, "volume.json", limits().max_bytes);
        if (!bytes.ok()) {
            return state_->protect(bytes.status().code());
        }
        auto marker = decode_marker(bytes.value());
        if (!marker.ok()) {
            return state_->protect(marker.status().code());
        }
        if (!(marker.value().id == state_->marker.id) ||
            marker.value().digest != state_->marker.digest) {
            return state_->protect(StatusCode::DataLoss);
        }
        return {};
    } catch (const std::bad_alloc&) {
        return state_->protect(StatusCode::ResourceExhausted);
    } catch (const std::filesystem::filesystem_error&) {
        return state_->protect(StatusCode::Unavailable);
    }
}

Result<std::uint64_t> ManagedVolume::available_bytes() const {
    auto verified = verify();
    if (!verified.ok()) {
        return verified.status();
    }
    struct statvfs space {};
    if (fstatvfs(state_->root_fd, &space) != 0) {
        return filesystem_failure();
    }
    if (space.f_frsize == 0 ||
        space.f_bavail > std::numeric_limits<std::uint64_t>::max() / space.f_frsize) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    return static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize;
}

Result<std::filesystem::path> ManagedVolume::create_staging(std::uint64_t segment_id) {
    if (segment_id == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto verified = verify();
    if (!verified.ok()) {
        return verified.status();
    }
    try {
        const auto name = std::to_string(segment_id) + ".tmp";
        const auto path = state_->artifacts / name;
        FileDescriptor parent(
            ::open(state_->artifacts.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (parent.get() < 0) {
            return state_->protect(StatusCode::Unavailable);
        }
        if (mkdirat(parent.get(), name.c_str(), 0700) != 0) {
            return errno == EEXIST ? Status(StatusCode::Conflict, {}) : filesystem_failure();
        }
        auto synced = persistence::sync_directory(state_->artifacts, state_->faults);
        if (!synced.ok()) {
            return synced.status();
        }
        verified = verify();
        if (!verified.ok()) {
            return verified.status();
        }
        return path;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
