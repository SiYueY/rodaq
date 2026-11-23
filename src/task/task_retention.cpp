#include "task_service_state.hpp"

#include <chrono>
#include <new>
#include <vector>

#include "../artifact/artifact_catalog.hpp"

namespace rodaq {

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
                work = entry->retention_work;
                current = std::atomic_load(&entry->published);
                if (!work && (entry->persisting || entry->controller->persistence_protected() ||
                              entry->control_work ||
                              current->config->retention_policy.mode != RetentionMode::Rolling)) {
                    continue;
                }
            }
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
                auto selected = catalog.select_retention(
                    entry->id, current->config->retention_policy, eligible);
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
                        entry->retention_work || latest->control != current->control ||
                        latest->config != current->config) {
                        continue;
                    }
                    work = task::RetentionWork{
                        *selected.value().candidate, current->snapshot.control_generation};
                    entry->retention_work = work;
                }
            }
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                if (!state_->accepting.load(std::memory_order_acquire)) {
                    break;
                }
                entry->retention_active = true;
            }
            struct ActualFlight {
                task::TaskEntry& entry;
                ~ActualFlight() {
                    std::lock_guard<std::mutex> control(entry.control_mutex);
                    entry.retention_active = false;
                }
            } flight{*entry};
            // Maintenance owns the actual call. A deadline only bounds observing
            // leases; it never discards the retained intent or releases disk charge.
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

}  // namespace rodaq
