#include "task_service_state.hpp"

#include "identity.hpp"
#include "../artifact/artifact_catalog.hpp"
#include "../persistence/atomic_file.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

namespace fs = std::filesystem;

struct ResetFlight {
    task::TaskEntry& entry;

    ~ResetFlight() noexcept {
        std::lock_guard<std::mutex> control(entry.control_mutex);
        entry.resetting = false;
    }
};

struct RetireFlight {
    task::TaskEntry& entry;
    std::atomic<std::size_t>& active_jobs;

    ~RetireFlight() noexcept {
        {
            std::lock_guard<std::mutex> control(entry.control_mutex);
            entry.retiring = false;
        }
        active_jobs.fetch_sub(1, std::memory_order_release);
    }
};

const task::StoredOperation* current_stored_operation(
    const task::TaskControlRecord& control) noexcept {
    if (!control.current_operation_id) {
        return nullptr;
    }
    for (const auto& entry : control.operation_history.entries) {
        if (entry.snapshot.operation_id == *control.current_operation_id) {
            return &entry;
        }
    }
    return nullptr;
}

Result<std::optional<task::TaskRecoveryRecord>> load_or_reconcile_recovery(task::TaskEntry& entry) {
    auto loaded = entry.recovery->load();
    if (loaded.ok()) {
        return std::optional<task::TaskRecoveryRecord>(std::move(loaded.value()));
    }
    if (loaded.status().code() == StatusCode::NotFound) {
        return std::optional<task::TaskRecoveryRecord>{};
    }
    if (loaded.status().code() == StatusCode::Unavailable) {
        return entry.recovery->reconcile();
    }
    return loaded.status();
}

Result<void> update_reset_recovery(
    task::TaskEntry& entry, const task::TaskControlRecord& control,
    const std::optional<task::ResetProgress>& progress) {
    if (!entry.recovery) {
        return {};
    }
    std::lock_guard<std::mutex> lock(entry.recovery_execution);
    auto existing = load_or_reconcile_recovery(entry);
    if (!existing.ok()) {
        return existing.status();
    }
    task::TaskRecoveryRecord record;
    std::uint64_t expected_revision = 0;
    if (existing.value()) {
        record = std::move(*existing.value());
        if (record.record_revision == std::numeric_limits<std::uint64_t>::max()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        expected_revision = record.record_revision++;
    } else {
        record.task_id = entry.id;
        record.record_revision = 1;
        record.recording_time_ns = 0;
    }
    if (control.control_generation > record.observed_control_generation) {
        record.observed_control_generation = control.control_generation;
    }
    record.active_config_version = control.active_config_version;
    record.reset_progress = progress;
    return entry.recovery->publish(record, expected_revision);
}

bool remove_file_if_present(const fs::path& path) noexcept {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) {
        return true;
    }
    if (error) {
        return false;
    }
    fs::remove(path, error);
    return !error;
}

bool remove_dir_tree_if_present(const fs::path& path) noexcept {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found) {
        return true;
    }
    if (error) {
        return false;
    }
    fs::remove_all(path, error);
    return !error;
}

bool path_absent(const fs::path& path) noexcept {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    return status.type() == fs::file_type::not_found;
}

}  // namespace

void TaskService::State::RemoveRetiredReceiptWork::run() noexcept {
    try {
        if (owner->reconcile_creates) {
            auto reconciled = owner->creates->reconcile();
            if (!reconciled.ok()) {
                reply->complete(reconciled.status());
                return;
            }
            owner->reconcile_creates = false;
        }
        auto removed = owner->creates->remove_for_retired_task(task_id);
        if (!removed.ok()) {
            owner->reconcile_creates = true;
        }
        reply->complete(std::move(removed));
    } catch (const std::bad_alloc&) {
        reply->complete(Status(StatusCode::ResourceExhausted, {}));
    } catch (...) {
        reply->complete(Status(StatusCode::Internal, {}));
    }
}

Result<void> TaskService::State::remove_retired_receipt(TaskId task_id) {
    try {
        auto reply = std::make_shared<task::Reply<bool>>();
        auto work = std::make_shared<RemoveRetiredReceiptWork>();
        work->owner = shared_from_this();
        work->task_id = task_id;
        work->reply = reply;
        auto submitted = options->create_lane->submit(std::move(work));
        if (!submitted.ok()) {
            return submitted.status();
        }
        auto result = reply->wait(options->request_wait);
        return result.ok() ? Result<void>{} : Result<void>(result.status());
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<bool> TaskService::State::active_namespace_absent(TaskId task_id) const {
    try {
        const auto id_text = to_string(task_id);
        const bool files_gone = path_absent(options->root / "tasks" / id_text) &&
                                path_absent(options->root / "control" / (id_text + ".json")) &&
                                path_absent(options->root / "control" / (id_text + ".lock")) &&
                                path_absent(options->root / "identities" / (id_text + ".json")) &&
                                path_absent(options->root / "identities" / (id_text + ".lock")) &&
                                path_absent(options->root / "recovery" / (id_text + ".json")) &&
                                path_absent(options->root / "recovery" / (id_text + ".lock"));
        if (!files_gone) {
            return false;
        }
        return true;
    } catch (...) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> TaskService::State::sync_active_parents() const {
    const auto hook = file_hook(task::ServiceFileDomain::Directories);
    for (const auto* name : {"tasks", "control", "identities", "recovery", "create"}) {
        auto synced = persistence::sync_directory(options->root / name, hook);
        if (!synced.ok()) {
            return synced.status();
        }
    }
    return {};
}

Result<bool> TaskService::State::cleanup_retired_namespace(TaskId task_id) {
    try {
        if (options->retire_faults &&
            options->retire_faults(task::RetireStep::NamespaceCleanup) != 0) {
            return Status(StatusCode::Unavailable, {});
        }
        const auto id_text = to_string(task_id);
        bool ok = true;
        ok = remove_dir_tree_if_present(options->root / "tasks" / id_text) && ok;
        ok = remove_file_if_present(options->root / "control" / (id_text + ".json")) && ok;
        ok = remove_file_if_present(options->root / "control" / (id_text + ".lock")) && ok;
        ok = remove_file_if_present(options->root / "identities" / (id_text + ".json")) && ok;
        ok = remove_file_if_present(options->root / "identities" / (id_text + ".lock")) && ok;
        ok = remove_file_if_present(options->root / "recovery" / (id_text + ".json")) && ok;
        ok = remove_file_if_present(options->root / "recovery" / (id_text + ".lock")) && ok;
        if (!ok) {
            return Status(StatusCode::Unavailable, {});
        }
        auto receipt_removed = remove_retired_receipt(task_id);
        if (!receipt_removed.ok()) {
            return receipt_removed.status();
        }
        auto synced = sync_active_parents();
        if (!synced.ok()) {
            return synced.status();
        }
        auto absent = active_namespace_absent(task_id);
        if (!absent.ok() || !absent.value()) {
            return absent.ok() ? Result<bool>(false) : absent;
        }
        {
            std::lock_guard<std::mutex> lock(retirement_mutex);
            pending_retirement_cleanups.erase(task_id);
        }
        return true;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Unavailable, {});
    }
}

void TaskService::State::start_reset(const std::shared_ptr<task::TaskEntry>& entry) noexcept {
    try {
        auto job = std::make_shared<ResetJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            const auto durable = entry->controller->durable_record();
            if (!accepting.load(std::memory_order_acquire) || !durable->reset_intent ||
                entry->resetting || !options->execution) {
                return;
            }
            entry->resetting = true;
            auto old = std::atomic_load(&entry->published);
            auto published = task::make_task_publication(
                *entry, old->config, old->snapshot.snapshot_sequence + 1);
            if (published.ok()) {
                std::atomic_store(&entry->published, std::move(published.value()));
            }
        }
        auto queued = enqueue_cleanup(entry, std::move(job), false);
        if (!queued.ok()) {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            entry->resetting = false;
        }
    } catch (...) {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        entry->resetting = false;
    }
}

void TaskService::State::ResetJob::run() noexcept {
    ResetFlight flight{*entry};
    try {
        std::shared_ptr<const task::TaskControlRecord> control;
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            control = entry->controller->durable_record();
        }
        if (!control->reset_intent) {
            return;
        }
        const auto intent = *control->reset_intent;
        const auto& artifacts = owner->options->execution->artifacts;

        const bool has_volume = std::any_of(
            artifacts.volume_id.value.begin(), artifacts.volume_id.value.end(),
            [](std::uint8_t b) { return b != 0; });
        if (has_volume) {
            task::ResetProgress initial_progress{
                intent.operation_id,
                intent.reset_generation,
                {task::VolumeResetProgress{artifacts.volume_id, false, false, false}}};
            auto recorded = update_reset_recovery(*entry, *control, initial_progress);
            if (!recorded.ok()) {
                return;
            }
        }

        if (owner->options->reset_faults &&
            owner->options->reset_faults(task::ResetStep::BeforeDelete) != 0) {
            return;
        }

        if (artifacts.reset_targets && artifacts.delete_for_reset) {
            auto targets = artifacts.reset_targets(entry->id, intent.target_control_generation);
            if (!targets.ok()) {
                return;
            }
            const auto deadline =
                std::chrono::steady_clock::now() + owner->options->execution->native_timeout;
            for (const auto target_id : targets.value()) {
                auto deleted = artifacts.delete_for_reset(
                    target_id, intent.reset_generation, task::control_wall_now(), deadline);
                if (!deleted.ok()) {
                    // Retain reset_intent and retry on subsequent maintenance or restart.
                    // Never settle Reset while ReadLease or deletion remains unproven.
                    return;
                }
            }
            if (artifacts.purge_quarantined_for_reset) {
                auto purged = artifacts.purge_quarantined_for_reset(
                    entry->id, intent.target_control_generation);
                if (!purged.ok()) {
                    return;
                }
            }
            auto remaining = artifacts.reset_targets(entry->id, intent.target_control_generation);
            if (!remaining.ok() || !remaining.value().empty()) {
                return;
            }
        }

        if (owner->options->reset_faults &&
            owner->options->reset_faults(task::ResetStep::AfterDeleteBeforeRecovery) != 0) {
            return;
        }

        if (has_volume) {
            task::ResetProgress settled_progress{
                intent.operation_id,
                intent.reset_generation,
                {task::VolumeResetProgress{artifacts.volume_id, true, true, true}}};
            auto recorded = update_reset_recovery(*entry, *control, settled_progress);
            if (!recorded.ok()) {
                return;
            }
            auto cleared = update_reset_recovery(*entry, *control, std::nullopt);
            if (!cleared.ok()) {
                return;
            }
        }

        if (owner->options->reset_faults &&
            owner->options->reset_faults(task::ResetStep::BeforeControlTerminal) != 0) {
            return;
        }

        owner->finish_execution(
            entry, intent.operation_id, control->control_generation, StatusCode::Ok, false,
            DataImpact::None);
    } catch (...) {
        // Durable reset_intent stays authoritative for retry or restart recovery.
    }
}

void TaskService::State::start_retire(const std::shared_ptr<task::TaskEntry>& entry) noexcept {
    try {
        auto job = std::make_shared<RetireJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        {
            std::lock_guard<std::mutex> control(entry->control_mutex);
            const auto durable = entry->controller->durable_record();
            const auto* current = current_stored_operation(*durable);
            if (!accepting.load(std::memory_order_acquire) || !current ||
                current->snapshot.kind != OperationKind::Retire ||
                task::terminal(current->snapshot.state) || entry->retiring || !options->execution) {
                return;
            }
            entry->retiring = true;
            auto old = std::atomic_load(&entry->published);
            auto published = task::make_task_publication(
                *entry, old->config, old->snapshot.snapshot_sequence + 1);
            if (published.ok()) {
                std::atomic_store(&entry->published, std::move(published.value()));
            }
        }
        active_retirement_jobs.fetch_add(1, std::memory_order_acq_rel);
        auto queued = enqueue_cleanup(entry, std::move(job), false);
        if (!queued.ok()) {
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                entry->retiring = false;
            }
            active_retirement_jobs.fetch_sub(1, std::memory_order_release);
        }
    } catch (...) {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        entry->retiring = false;
    }
}

void TaskService::State::RetireJob::run() noexcept {
    RetireFlight flight{*entry, owner->active_retirement_jobs};
    try {
        std::shared_ptr<const task::TaskControlRecord> control;
        bool task_side_ready = false;
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            control = entry->controller->durable_record();
            task_side_ready = control->desired_state == DesiredState::Stopped &&
                              !control->reset_intent && entry->quiescent && entry->closing == 0 &&
                              !entry->retention_work && !entry->artifact_maintenance_active &&
                              !entry->resetting;
        }
        const auto* current = current_stored_operation(*control);
        if (!current || current->snapshot.kind != OperationKind::Retire ||
            task::terminal(current->snapshot.state)) {
            return;
        }
        const auto op_id = current->snapshot.operation_id;
        const auto control_gen = control->control_generation;

        if (!task_side_ready) {
            owner->finish_execution(
                entry, op_id, control_gen, StatusCode::InvalidState, false, DataImpact::None);
            return;
        }

        const auto& artifacts = owner->options->execution->artifacts;
        if (artifacts.retire_eligible) {
            auto eligible = artifacts.retire_eligible(entry->id);
            if (!eligible.ok() || !eligible.value()) {
                owner->finish_execution(
                    entry, op_id, control_gen,
                    eligible.ok() ? StatusCode::Conflict : eligible.status().code(), false,
                    DataImpact::None);
                return;
            }
        }

        if (owner->options->retire_faults &&
            owner->options->retire_faults(task::RetireStep::BeforeTombstone) != 0) {
            // Simulated crash before tombstone: Retire stays non-terminal, never Succeeded.
            return;
        }

        task::TaskRetirementRecord tombstone;
        tombstone.task_id = entry->id;
        tombstone.retire_operation_id = op_id;
        tombstone.request = current->request;
        tombstone.request_digest = current->request_digest;
        tombstone.last_control_generation = control_gen;
        tombstone.last_config_version = control->active_config_version;
        tombstone.retired_at = std::max(task::control_wall_now(), current->snapshot.accepted_at);

        auto published = owner->retired->publish(tombstone);
        if (!published.ok()) {
            // Reconcile uncertain write: if tombstone IS durable, continue as Succeeded.
            auto reconciled = owner->retired->reconcile();
            auto present = reconciled.ok() ? owner->retired->contains(entry->id)
                                           : Result<bool>(reconciled.status());
            if (!present.ok() || !present.value()) {
                // Tombstone is NOT durable: never report Succeeded.
                return;
            }
        }

        // Durable retirement tombstone is the irreversible Retire Succeeded
        // linearization point. Remove from active catalog and release max_tasks
        // immediately; post-retirement namespace cleanup runs independently.
        {
            std::lock_guard<std::mutex> catalog_lock(owner->catalog_mutex);
            const auto found = std::find(owner->entries.begin(), owner->entries.end(), entry);
            if (found != owner->entries.end()) {
                owner->entries.erase(found);
            }
        }
        {
            std::lock_guard<std::mutex> lock(owner->retirement_mutex);
            owner->pending_retirement_cleanups.insert(entry->id);
        }

        // Release exclusive file locks owned by the retired Task entry before
        // unlinking its old active namespace files.
        {
            std::lock_guard<std::mutex> control_lock(entry->control_mutex);
            entry->store.reset();
            entry->identities.reset();
        }
        {
            std::lock_guard<std::mutex> recovery_lock(entry->recovery_execution);
            entry->recovery.reset();
        }

        if (owner->options->retire_faults &&
            owner->options->retire_faults(task::RetireStep::AfterTombstone) != 0) {
            // Simulated crash immediately after tombstone: Task is already
            // permanently retired and Succeeded; cleanup remains pending.
            return;
        }

        (void)owner->cleanup_retired_namespace(entry->id);
    } catch (...) {
        // Pre-tombstone exceptions leave Retire non-terminal; post-tombstone
        // exceptions leave pending_retirement_cleanups for maintenance retry.
    }
}

Result<void> TaskService::reconcile_destructive(
    artifact::ArtifactCatalog& catalog, std::chrono::nanoseconds audit_window) {
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return {};
    }
    try {
        Status first_failure;

        // 1. Retry any active Task with pending durable Reset or Retire intent.
        std::vector<std::shared_ptr<task::TaskEntry>> active_entries;
        {
            std::lock_guard<std::mutex> lock(state_->catalog_mutex);
            active_entries = state_->entries;
        }
        for (const auto& entry : active_entries) {
            bool needs_reset = false;
            bool needs_retire = false;
            {
                std::lock_guard<std::mutex> control(entry->control_mutex);
                const auto durable = entry->controller->durable_record();
                if (durable->reset_intent && !entry->resetting && !entry->persisting &&
                    !entry->controller->persistence_protected()) {
                    needs_reset = true;
                } else {
                    const auto* op = current_stored_operation(*durable);
                    if (op && op->snapshot.kind == OperationKind::Retire &&
                        !task::terminal(op->snapshot.state) && !entry->retiring &&
                        !entry->persisting && !entry->controller->persistence_protected()) {
                        needs_retire = true;
                    }
                }
            }
            if (needs_reset) {
                state_->start_reset(entry);
            } else if (needs_retire) {
                state_->start_retire(entry);
            }
        }

        // 2. Retry any pending post-retirement active namespace cleanups.
        std::vector<TaskId> pending_cleanups;
        {
            std::lock_guard<std::mutex> lock(state_->retirement_mutex);
            pending_cleanups.assign(
                state_->pending_retirement_cleanups.begin(),
                state_->pending_retirement_cleanups.end());
        }
        for (const auto task_id : pending_cleanups) {
            auto cleaned = state_->cleanup_retired_namespace(task_id);
            if (!cleaned.ok() && first_failure.ok()) {
                first_failure = cleaned.status();
            }
        }

        // 3. Fail-closed retirement tombstone GC.
        auto tombstones = state_->retired->list();
        if (!tombstones.ok()) {
            return tombstones.status();
        }
        for (const auto& tombstone : tombstones.value()) {
            bool pending_maint = false;
            {
                std::lock_guard<std::mutex> lock(state_->retirement_mutex);
                pending_maint = state_->pending_retirement_cleanups.count(tombstone.task_id) != 0;
            }
            if (pending_maint) {
                continue;
            }
            auto absent = state_->active_namespace_absent(tombstone.task_id);
            if (!absent.ok() || !absent.value()) {
                continue;
            }
            auto has_refs = catalog.has_task_references(tombstone.task_id);
            if (!has_refs.ok() || has_refs.value()) {
                continue;
            }
            // Only fsync active parents and attempt GC once audit window has elapsed.
            const auto now = task::control_wall_now();
            const auto retired_ns = tombstone.retired_at.time_since_epoch().count();
            const auto now_ns = now.time_since_epoch().count();
            if (now_ns < retired_ns) {
                continue;
            }
            const auto elapsed =
                static_cast<std::uint64_t>(now_ns) - static_cast<std::uint64_t>(retired_ns);
            if (elapsed < static_cast<std::uint64_t>(audit_window.count())) {
                continue;
            }
            auto synced = state_->sync_active_parents();
            if (!synced.ok()) {
                if (first_failure.ok()) {
                    first_failure = synced.status();
                }
                continue;
            }
            task::RetirementCollectProof proof{now, audit_window, true, true, true};
            auto collected = state_->retired->collect(tombstone.task_id, proof);
            if (!collected.ok() && collected.status().code() != StatusCode::InvalidState &&
                first_failure.ok()) {
                first_failure = collected.status();
            }
        }
        return first_failure.ok() ? Result<void>{} : Result<void>(first_failure);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
