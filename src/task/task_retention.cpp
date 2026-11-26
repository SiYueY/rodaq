#include "task_service_state.hpp"

#include <chrono>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "../artifact/artifact_catalog.hpp"

namespace rodaq {
namespace {

struct ArtifactMaintenanceFlight {
    task::TaskEntry& entry;
    ~ArtifactMaintenanceFlight() {
        std::lock_guard<std::mutex> control(entry.control_mutex);
        entry.artifact_maintenance_active = false;
    }
};

Result<task::TaskRecoveryRecord> retention_recovery(task::TaskEntry& entry) {
    auto record = entry.recovery->load();
    if (!record.ok() && record.status().code() == StatusCode::Unavailable) {
        auto actual = entry.recovery->reconcile();
        if (!actual.ok()) {
            return actual.status();
        }
        return actual.value() ? Result<task::TaskRecoveryRecord>(std::move(*actual.value()))
                              : Result<task::TaskRecoveryRecord>(Status(StatusCode::NotFound, {}));
    }
    return record;
}

Result<std::optional<std::uint64_t>> committed_floor(task::TaskEntry& entry) {
    std::lock_guard<std::mutex> recovery(entry.recovery_execution);
    auto record = retention_recovery(entry);
    if (!record.ok()) {
        return record.status().code() == StatusCode::NotFound
                   ? Result<std::optional<std::uint64_t>>(std::optional<std::uint64_t>{})
                   : Result<std::optional<std::uint64_t>>(record.status());
    }
    // A missing legacy checkpoint is a conservative lower bound, never silent
    // Open time or permission to infer an unverified committed tail.
    return record.value().committed_recording_end_ns;
}

Result<void> checkpoint_committed(task::TaskEntry& entry, std::uint64_t boundary) {
    std::lock_guard<std::mutex> recovery(entry.recovery_execution);
    auto record = retention_recovery(entry);
    if (!record.ok()) {
        return record.status().code() == StatusCode::NotFound ? Status(StatusCode::DataLoss, {})
                                                              : record.status();
    }
    if (record.value().committed_recording_end_ns &&
        *record.value().committed_recording_end_ns >= boundary) {
        return {};
    }
    if (record.value().record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    const auto expected = record.value().record_revision++;
    record.value().committed_recording_end_ns = boundary;
    return entry.recovery->publish(record.value(), expected);
}

}  // namespace

Result<void> TaskService::apply_retention(artifact::ArtifactCatalog& catalog) {
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return {};
    }
    try {
        std::vector<std::shared_ptr<task::TaskEntry>> entries;
        {
            std::lock_guard<std::mutex> lock(state_->catalog_mutex);
            entries = state_->entries;
        }
        Status first_failure;
        for (const auto& entry : entries) {
            std::shared_ptr<const task::PublishedTask> current;
            std::optional<task::RetentionWork> work;
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                if (!state_->accepting.load(std::memory_order_acquire)) {
                    break;
                }
                if (entry->artifact_maintenance_active || entry->retiring || entry->resetting ||
                    !entry->store || !entry->recovery) {
                    continue;
                }
                work = entry->retention_work;
                current = std::atomic_load(&entry->published);
                if (!work && (entry->persisting || entry->controller->persistence_protected() ||
                              entry->control_work || current->control->reset_intent ||
                              current->config->retention_policy.mode != RetentionMode::Rolling)) {
                    continue;
                }
                entry->artifact_maintenance_active = true;
            }
            ArtifactMaintenanceFlight flight{*entry};
            if (!work) {
                auto versions = catalog.retention_versions(entry->id);
                if (!versions.ok()) {
                    if (first_failure.ok()) {
                        first_failure = versions.status();
                    }
                    continue;
                }
                std::vector<std::string> eligible;
                bool resolved = true;
                for (const auto& version : versions.value()) {
                    auto historical = entry->store->load_config(version);
                    if (!historical.ok()) {
                        if (first_failure.ok()) {
                            first_failure = historical.status();
                        }
                        resolved = false;
                        break;
                    }
                    if (historical.value().config.recording_mode == RecordingMode::Rolling ||
                        current->config->retention_policy.include_previous_continuous) {
                        eligible.push_back(version);
                    }
                }
                if (!resolved) {
                    continue;
                }
                auto floor = committed_floor(*entry);
                if (!floor.ok()) {
                    if (first_failure.ok()) {
                        first_failure = floor.status();
                    }
                    continue;
                }
                auto selected = catalog.select_retention(
                    entry->id, current->config->retention_policy, eligible,
                    floor.value().value_or(0));
                if (!selected.ok()) {
                    if (first_failure.ok()) {
                        first_failure = selected.status();
                    }
                    continue;
                }
                if (!selected.value().candidate) {
                    continue;
                }
                {
                    std::lock_guard<std::mutex> control(entry->control_mutex);
                    const auto latest = std::atomic_load(&entry->published);
                    if (!state_->accepting.load(std::memory_order_acquire) || entry->persisting ||
                        entry->controller->persistence_protected() || entry->control_work ||
                        entry->retention_work || entry->retiring || entry->resetting ||
                        latest->control != current->control || latest->config != current->config) {
                        continue;
                    }
                    work = task::RetentionWork{
                        *selected.value().candidate, current->snapshot.control_generation,
                        selected.value().committed_recording_end_ns};
                    entry->retention_work = work;
                }
            }
            // Maintenance owns the actual call. A deadline only bounds observing
            // leases; it never discards the retained intent or releases disk charge.
            auto checkpoint = checkpoint_committed(*entry, work->committed_recording_end_ns);
            if (!checkpoint.ok()) {
                if (first_failure.ok()) {
                    first_failure = checkpoint.status();
                }
                continue;
            }
            auto deleted = catalog.delete_artifact(
                work->artifact, work->generation, artifact::DeleteReason::Retention,
                task::control_wall_now(), std::chrono::steady_clock::now());
            if (deleted.ok()) {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                entry->retention_work.reset();
            } else if (first_failure.ok()) {
                first_failure = deleted.status();
            }
        }
        return first_failure.ok() ? Result<void>{} : Result<void>(first_failure);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> TaskService::collect_artifacts(
    artifact::ArtifactCatalog& catalog, std::chrono::nanoseconds audit_window) {
    try {
        std::vector<std::shared_ptr<task::TaskEntry>> entries;
        {
            std::lock_guard<std::mutex> lock(state_->catalog_mutex);
            entries = state_->entries;
        }
        Status first_failure;
        for (const auto& entry : entries) {
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                if (!state_->accepting.load(std::memory_order_acquire)) {
                    break;
                }
                if (entry->artifact_maintenance_active || entry->retiring || entry->resetting ||
                    !entry->recovery) {
                    continue;
                }
                entry->artifact_maintenance_active = true;
            }
            ArtifactMaintenanceFlight flight{*entry};
            auto floor = committed_floor(*entry);
            if (!floor.ok()) {
                if (first_failure.ok()) {
                    first_failure = floor.status();
                }
                continue;
            }
            if (!floor.value()) {
                // Retention selection may use a conservative zero lower bound;
                // GC must have an actual durable checkpoint, including known zero.
                continue;
            }
            auto collected = catalog.collect_deleted(
                entry->id, *floor.value(), task::control_wall_now(), audit_window);
            if (!collected.ok() && first_failure.ok()) {
                first_failure = collected.status();
            }
        }
        return first_failure.ok() ? Result<void>{} : Result<void>(first_failure);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
