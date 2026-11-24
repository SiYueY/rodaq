#include "startup.hpp"

#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "../persistence/atomic_file.hpp"
#include "../persistence/json.hpp"
#include "../artifact/recovery_scan.hpp"
#include "../artifact/segment_committer.hpp"
#include "run_configuration.hpp"

namespace rodaq::daq {
namespace {

constexpr auto kLock = ".daq.lock";
constexpr auto kMarker = "bootstrap.json";
constexpr auto kDomain = "rodaq.deployment-bootstrap.v1";

}  // namespace

Result<void> recover_pending_artifacts(const CompositionOptions& options) {
    auto records = options.ledger->recovery_snapshot();
    if (!records.ok()) {
        return records.status();
    }
    const auto open_task = [&](TaskId id) {
        return task::TaskStore::open(
            options.task_root / "tasks" / to_string(id), options.task_root / "control", id,
            task::TaskStoreMode::Existing, options.settings.tasks);
    };
    // Durable DeleteIntent wins before any committing publication recovery.
    for (const auto& record : records.value()) {
        if (record.state != artifact::LedgerState::Deleting) {
            continue;
        }
        auto store = open_task(record.task_id);
        if (!store.ok()) {
            return store.status();
        }
        auto control = store.value()->load_control();
        if (!control.ok()) {
            return control.status();
        }
        if (record.control_generation > control.value().control_generation) {
            return Status(StatusCode::Conflict, {});
        }
        auto deleted = artifact::recover_deleting_artifact(
            options.volume, *options.ledger, record.segment_id,
            catalog_manifest_limits(options.settings));
        if (!deleted.ok()) {
            return deleted.status();
        }
    }
    const auto authorize = [&](const artifact::LedgerRecord& record,
                               const artifact::SegmentManifest& manifest) -> Result<void> {
        auto store = open_task(record.task_id);
        if (!store.ok()) {
            return store.status();
        }
        auto control = store.value()->load_control();
        if (!control.ok()) {
            return control.status();
        }
        if (record.control_generation > control.value().control_generation ||
            record.reset_generation_at_commit != control.value().reset_generation ||
            control.value().reset_intent) {
            return Status(StatusCode::Conflict, {});
        }
        auto config = store.value()->load_config(manifest.identity.config_version);
        if (!config.ok()) {
            return config.status();
        }
        const auto pipeline = std::find_if(
            config.value().config.pipelines.begin(), config.value().config.pipelines.end(),
            [&](const auto& selected) { return selected.pipeline_id == record.pipeline_id; });
        if (pipeline == config.value().config.pipelines.end() ||
            pipeline->acquisition.plugin_id != manifest.identity.acquisition.plugin_id ||
            pipeline->acquisition.plugin_version != manifest.identity.acquisition.plugin_version ||
            pipeline->storage.plugin_id != manifest.identity.storage.plugin_id ||
            pipeline->storage.plugin_version != manifest.identity.storage.plugin_version) {
            return Status(StatusCode::DataLoss, {});
        }
        return {};
    };
    for (const auto& record : records.value()) {
        if (record.state != artifact::LedgerState::Committing) {
            continue;
        }
        auto recovered = artifact::SegmentCommitter::recover(
            options.volume, *options.ledger, record.segment_id,
            catalog_manifest_limits(options.settings), authorize);
        if (!recovered.ok()) {
            return recovered.status();
        }
    }
    return {};
}

namespace {

struct StartupPlugins {
    std::shared_ptr<plugin::PluginManager> owner;
    plugin::DeadlineDuration timeout;

    ~StartupPlugins() noexcept {
        if (!owner) {
            return;
        }
        try {
            const auto deadline = plugin::Deadline::after(timeout);
            if (deadline.ok()) {
                (void)owner->shutdown(deadline.value());
            }
        } catch (...) {
            // Preserve the startup error. PluginManager still owns its actual
            // framework state and retains its abandoned-owner drain fallback.
        }
    }
};

Result<std::shared_ptr<DeploymentLease>> acquire(
    const std::filesystem::path& root, DeploymentOpenMode mode) {
    persistence::FileDescriptor directory(
        ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0) {
        return Status(StatusCode::Unavailable, {});
    }
    persistence::FileDescriptor lock(::openat(
        directory.get(), kLock,
        O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
            (mode == DeploymentOpenMode::Initialize ? O_CREAT : 0),
        0600));
    struct stat directory_stat {};
    struct stat lock_stat {};
    if (lock.get() < 0 || ::fstat(directory.get(), &directory_stat) != 0 ||
        ::fstat(lock.get(), &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode) ||
        lock_stat.st_nlink != 1) {
        return Status(StatusCode::DataLoss, {});
    }
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
        return Status(errno == EWOULDBLOCK ? StatusCode::Conflict : StatusCode::Unavailable, {});
    }
    auto lease = std::make_shared<DeploymentLease>(std::move(directory), std::move(lock));
    lease->root = root;
    lease->device = directory_stat.st_dev;
    lease->inode = directory_stat.st_ino;
    lease->lock_inode = lock_stat.st_ino;
    return lease;
}

Result<void> directory(const std::filesystem::path& path, DeploymentOpenMode mode) {
    if (mode == DeploymentOpenMode::Initialize) {
        if (!std::filesystem::create_directory(path)) {
            return Status(StatusCode::Conflict, {});
        }
        auto synced = persistence::sync_directory(path);
        if (!synced.ok()) {
            return synced.status();
        }
        return persistence::sync_directory(path.parent_path());
    }
    if (std::filesystem::symlink_status(path).type() != std::filesystem::file_type::directory) {
        return Status(StatusCode::DataLoss, {});
    }
    return {};
}

Result<artifact::VolumeId> existing_volume(const std::filesystem::path& root) {
    auto marker = persistence::read_file(root, kMarker, 1024);
    if (!marker.ok()) {
        return marker.status().code() == StatusCode::NotFound ? Status(StatusCode::DataLoss, {})
                                                              : marker.status();
    }
    auto decoded = persistence::parse_json(marker.value(), {1024, 2, 128, 4, 0, 5});
    if (!decoded.ok()) {
        return Status(StatusCode::DataLoss, {});
    }
    const auto& value = decoded.value();
    if (!value.is_object() || value.size() != 3 || !value.contains("format_version") ||
        value["format_version"] != "1.0" || !value.contains("volume_id") ||
        !value["volume_id"].is_string() || !value.contains("content_digest")) {
        return Status(StatusCode::DataLoss, {});
    }
    auto verified = persistence::verify_integrity(value, kDomain);
    if (!verified.ok()) {
        return verified.status();
    }
    auto volume = artifact::parse_volume_id(value["volume_id"].get_ref<const std::string&>());
    return volume.ok() ? volume : Result<artifact::VolumeId>(Status(StatusCode::DataLoss, {}));
}

}  // namespace

DeploymentLease::DeploymentLease(
    persistence::FileDescriptor directory_fd, persistence::FileDescriptor lock_fd)
: directory(std::move(directory_fd)), lock(std::move(lock_fd)) {}

Result<void> DeploymentLease::verify() const {
    struct stat directory_stat {};
    struct stat path_stat {};
    struct stat lock_stat {};
    struct stat lock_path_stat {};
    if (::fstat(directory.get(), &directory_stat) != 0 || ::lstat(root.c_str(), &path_stat) != 0 ||
        ::fstat(lock.get(), &lock_stat) != 0 ||
        ::fstatat(directory.get(), kLock, &lock_path_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(path_stat.st_mode) || path_stat.st_dev != device || path_stat.st_ino != inode ||
        directory_stat.st_dev != device || directory_stat.st_ino != inode ||
        !S_ISREG(lock_stat.st_mode) || lock_stat.st_nlink != 1 || lock_stat.st_dev != device ||
        lock_stat.st_ino != lock_inode || lock_path_stat.st_dev != device ||
        lock_path_stat.st_ino != lock_inode) {
        return Status(StatusCode::DataLoss, {});
    }
    return {};
}

Result<std::unique_ptr<Composition>> Composition::open(StartupOptions startup) {
    try {
        if (!startup.state_root.is_absolute() || !startup.volume_root.is_absolute() ||
            startup.ledger_records == 0 || startup.ledger_records > 65536 ||
            (startup.mode != DeploymentOpenMode::Initialize &&
             startup.mode != DeploymentOpenMode::Existing)) {
            return Status(StatusCode::InvalidArgument, {});
        }
        auto lease = acquire(startup.state_root, startup.mode);
        if (!lease.ok()) {
            return lease.status();
        }
        const bool initialize = startup.mode == DeploymentOpenMode::Initialize;
        if (initialize) {
            for (const auto& entry : std::filesystem::directory_iterator(startup.state_root)) {
                if (entry.path().filename() != kLock) {
                    return Status(StatusCode::Conflict, {});
                }
            }
            if (std::filesystem::symlink_status(startup.volume_root).type() !=
                    std::filesystem::file_type::directory ||
                !std::filesystem::is_empty(startup.volume_root)) {
                return Status(StatusCode::Conflict, {});
            }
        }
        artifact::VolumeId volume_id;
        if (!initialize) {
            auto recorded = existing_volume(startup.state_root);
            if (!recorded.ok()) {
                return recorded.status();
            }
            volume_id = recorded.value();
        }
        for (const auto* name : {"service", "time", "segment-ids", "ledger"}) {
            auto prepared = directory(startup.state_root / name, startup.mode);
            if (!prepared.ok()) {
                return prepared.status();
            }
        }
        if (!initialize) {
            const auto retired = startup.state_root / "service/retired";
            const auto retired_type = std::filesystem::symlink_status(retired).type();
            if (retired_type != std::filesystem::file_type::not_found &&
                (retired_type != std::filesystem::file_type::directory ||
                 !std::filesystem::is_empty(retired))) {
                // No ordinary Task/artifact recovery may precede retirement
                // authority. Retain this diagnostic until its registry opens.
                return Status(StatusCode::Unavailable, {});
            }
            for (const auto* name : {"tasks", "control", "identities", "create", "recovery"}) {
                auto checked = directory(startup.state_root / "service" / name, startup.mode);
                if (!checked.ok()) {
                    return checked.status();
                }
            }
        }
        CompositionOptions options;
        options.task_root = startup.state_root / "service";
        options.settings = std::move(startup.settings);
        auto volume = artifact::ManagedVolume::open(
            startup.volume_root,
            initialize ? artifact::VolumeOpenMode::Initialize : artifact::VolumeOpenMode::Existing,
            volume_id);
        if (!volume.ok()) {
            return volume.status();
        }
        options.volume = std::move(volume.value());
        auto ledger = artifact::LedgerStore::open(
            startup.state_root / "ledger",
            initialize ? artifact::LedgerOpenMode::Initialize : artifact::LedgerOpenMode::Existing,
            startup.ledger_records);
        if (!ledger.ok()) {
            return ledger.status();
        }
        options.ledger = std::move(ledger.value());
        auto pending = recover_pending_artifacts(options);
        if (!pending.ok()) {
            return pending.status();
        }
        auto usage = artifact::inspect_committed_volume(
            options.volume, *options.ledger, catalog_manifest_limits(options.settings));
        if (!usage.ok()) {
            return usage.status();
        }
        auto ids = artifact::SegmentIdStore::open(
            startup.state_root / "segment-ids", initialize
                                                    ? artifact::SegmentIdOpenMode::NewDeployment
                                                    : artifact::SegmentIdOpenMode::Existing);
        if (!ids.ok()) {
            return ids.status();
        }
        options.segment_ids = std::move(ids.value());
        auto capacity = artifact::VolumeCapacity::create(
            startup.capacity, usage.value().physical_bytes, usage.value().artifact_slots);
        if (!capacity.ok()) {
            return capacity.status();
        }
        options.capacity = std::make_shared<artifact::VolumeCapacity>(std::move(capacity.value()));
        auto boot = time::read_boot_identity();
        auto sample = time::sample_clocks();
        if (!boot.ok() || !sample.ok()) {
            return !boot.ok() ? boot.status() : sample.status();
        }
        options.boot = boot.value();
        auto clock = time::ClockCorrelation::create(
            startup.state_root / "time", options.boot, sample.value(),
            {initialize ? time::BootArtifactState::Absent : time::BootArtifactState::Unknown, {}});
        if (!clock.ok()) {
            return clock.status();
        }
        options.clock = std::move(clock.value());
        const auto plugin_timeout = startup.plugins.shutdown_timeout;
        auto plugins = plugin::PluginManager::create(std::move(startup.plugins));
        if (!plugins.ok()) {
            return plugins.status();
        }
        options.plugins = std::make_shared<plugin::PluginManager>(std::move(plugins.value()));
        StartupPlugins pending_plugins{options.plugins, plugin_timeout};
        auto checked = lease.value()->verify();
        if (!checked.ok()) {
            return checked.status();
        }
        if (initialize) {
            auto encoded = persistence::encode_integrity(
                {{"format_version", "1.0"},
                 {"volume_id", artifact::to_string(options.volume->identity())}},
                kDomain);
            if (!encoded.ok()) {
                return encoded.status();
            }
            auto published = persistence::publish_file(
                startup.state_root, kMarker, encoded.value(), persistence::FilePublishMode::Create,
                1024);
            if (!published.status.ok()) {
                return published.status;
            }
        }
        auto composition = create(std::move(options));
        if (!composition.ok()) {
            return composition.status();
        }
        composition.value()->retain_lease(std::move(lease.value()));
        pending_plugins.owner.reset();
        return composition;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const std::filesystem::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

}  // namespace rodaq::daq
