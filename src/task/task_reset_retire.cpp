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
#include <optional>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

namespace fs = std::filesystem;

const task::StoredOperation* find_current_op(const task::TaskControlRecord& control) noexcept {
    if (!control.current_operation_id) {
        return nullptr;
    }
    const auto it = std::find_if(
        control.operation_history.entries.begin(), control.operation_history.entries.end(),
        [&](const auto& item) {
            return item.snapshot.operation_id == *control.current_operation_id;
        });
    return it == control.operation_history.entries.end() ? nullptr : &*it;
}

bool audit_window_elapsed(WallTime retired_at, WallTime now, std::chrono::nanoseconds window) {
    if (now < retired_at) {
        return false;
    }
    const auto elapsed = static_cast<std::uint64_t>(now.time_since_epoch().count()) -
                         static_cast<std::uint64_t>(retired_at.time_since_epoch().count());
    return elapsed >= static_cast<std::uint64_t>(window.count());
}

Result<void> record_reset_progress(
    task::TaskEntry& entry, const task::TaskControlRecord& control, const task::ResetIntent& intent,
    artifact::VolumeId volume_id, bool files_removed, bool ledger_settled, bool catalog_settled) {
    if (!entry.recovery) {
        return {};
    }
    std::lock_guard<std::mutex> recovery(entry.recovery_execution);
    auto loaded = entry.recovery->load();
    if (!loaded.ok() && loaded.status().code() == StatusCode::Unavailable) {
        auto reconciled = entry.recovery->reconcile();
        if (!reconciled.ok()) {
            return reconciled.status();
        }
        loaded = reconciled.value()
                     ? Result<task::TaskRecoveryRecord>(std::move(*reconciled.value()))
                     : Result<task::TaskRecoveryRecord>(Status(StatusCode::NotFound, {}));
    }
    task::TaskRecoveryRecord record;
    std::uint64_t expected = 0;
    if (loaded.ok()) {
        record = std::move(loaded.value());
        if (record.record_revision == std::numeric_limits<std::uint64_t>::max()) {
            return Status(StatusCode::ResourceExhausted, {});
        }
        expected = record.record_revision++;
    } else if (loaded.status().code() != StatusCode::NotFound) {
        return loaded.status();
    }
    record.task_id = entry.id;
    record.observed_control_generation = control.control_generation;
    record.active_config_version = control.active_config_version;
    if (files_removed && ledger_settled && catalog_settled) {
        record.reset_progress.reset();
        record.run_operation.reset();
        record.pipelines.clear();
        record.cleanup_intents.clear();
    } else {
        task::ResetProgress progress;
        progress.operation_id = intent.operation_id;
        progress.reset_generation = intent.reset_generation;
        progress.volumes.push_back({volume_id, files_removed, ledger_settled, catalog_settled});
        record.reset_progress = std::move(progress);
    }
    return entry.recovery->publish(record, expected);
}

Result<void> end_stale_boot_run(
    task::TaskEntry& entry, const task::TaskControlRecord& control, bool revoke_continuation) {
    if (!entry.recovery) {
        return {};
    }
    std::lock_guard<std::mutex> recovery(entry.recovery_execution);
    auto loaded = entry.recovery->load();
    if (!loaded.ok()) {
        return loaded.status().code() == StatusCode::NotFound ? Result<void>{}
                                                              : Result<void>(loaded.status());
    }
    auto record = std::move(loaded.value());
    bool changed = false;
    if (record.run_operation && record.run_operation->phase != task::RunPhase::Ended) {
        for (auto& pipeline : record.run_operation->authorized_pipelines) {
            if (revoke_continuation) {
                pipeline.continuation_permission = false;
            }
            if (!pipeline.ended_boundary) {
                pipeline.ended_boundary = record.run_operation->authorization_boundary
                                              ? *record.run_operation->authorization_boundary
                                              : pipeline.prepared_boundary;
            }
        }
        record.run_operation->phase = task::RunPhase::Ended;
        changed = true;
    }
    if (revoke_continuation) {
        for (auto& pipeline : record.pipelines) {
            if (pipeline.instance.continuation_permission) {
                pipeline.instance.continuation_permission = false;
                changed = true;
            }
        }
    }
    if (!changed) {
        return {};
    }
    if (record.record_revision == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::ResourceExhausted, {});
    }
    record.observed_control_generation =
        std::max(record.observed_control_generation, control.control_generation);
    record.active_config_version = control.active_config_version;
    const auto expected = record.record_revision++;
    return entry.recovery->publish(record, expected);
}

Result<void> publish_terminal_sync(
    const TaskConfigLimits& config_limits, const std::shared_ptr<task::TaskEntry>& entry,
    const OperationSnapshot& terminal_op, bool set_quiescent) {
    std::shared_ptr<const task::ControlWork> work;
    {
        std::lock_guard<std::mutex> control(entry->control_mutex);
        auto candidate = entry->controller->update_operation(terminal_op);
        if (!candidate.ok()) {
            return candidate.status();
        }
        work = std::move(candidate.value());
        entry->control_work = work;
        entry->persisting = true;
    }
    auto actual = task::execute_control_work(*entry->store, *work, nullptr, config_limits);
    std::lock_guard<std::mutex> control(entry->control_mutex);
    auto completed = entry->controller->persistence_completed(
        actual.work_id, actual.authority, actual.publication_failure);
    entry->persisting = false;
    entry->control_work.reset();
    if (!completed.ok()) {
        return completed.status();
    }
    if (!completed.value().resolved || !completed.value().resolved->durable) {
        return completed.value().resolved ? completed.value().resolved->status
                                          : Status(StatusCode::Unavailable, {});
    }
    if (set_quiescent) {
        entry->quiescent = true;
        entry->resetting = false;
    }
    auto old = std::atomic_load(&entry->published);
    auto fresh =
        task::make_task_publication(*entry, old->config, old->snapshot.snapshot_sequence + 1);
    if (fresh.ok()) {
        std::atomic_store(&entry->published, std::move(fresh.value()));
    }
    return {};
}

}  // namespace

Result<OperationId> TaskService::reset(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    if (state_->retirements && state_->retirements->is_retired(id)) {
        return Status(StatusCode::NotFound, {});
    }
    return lifecycle(id, OperationKind::Reset, request, precondition);
}

Result<OperationId> TaskService::retire(
    TaskId id, const RequestIdentity& request, const ControlPrecondition& precondition) {
    if (!state_->accepting.load(std::memory_order_acquire)) {
        return Status(StatusCode::InvalidState, {});
    }
    auto valid_req = task::validate_request_identity(request);
    if (!valid_req.ok()) {
        return valid_req.status();
    }
    auto digest = task::lifecycle_request_digest(id, OperationKind::Retire, precondition);
    if (!digest.ok()) {
        return digest.status();
    }
    if (state_->retirements) {
        auto tombstone = state_->retirements->find(id);
        if (tombstone) {
            if (tombstone->request_key == request.request_key) {
                if (tombstone->request_digest == digest.value()) {
                    return tombstone->operation_id;
                }
                return Status(StatusCode::Conflict, {});
            }
            return Status(StatusCode::NotFound, {});
        }
    }
    return lifecycle(id, OperationKind::Retire, request, precondition);
}

void TaskService::State::ResetJob::run() noexcept {
    struct ResetGuard {
        std::shared_ptr<task::TaskEntry> entry;
        ~ResetGuard() {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            entry->resetting = false;
        }
    } guard{entry};

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
        const auto* op = find_current_op(*control);
        if (!op || task::terminal(op->snapshot.state)) {
            return;
        }

        auto catalog = owner->options->catalog;
        if (!catalog) {
            // Standalone TaskService without ArtifactCatalog: settle Reset cleanly.
            auto terminal_op = op->snapshot;
            terminal_op.state = OperationState::Succeeded;
            terminal_op.terminal_at = std::max(task::control_wall_now(), terminal_op.accepted_at);
            terminal_op.status_code = StatusCode::Ok;
            (void)publish_terminal_sync(owner->options->store.config, entry, terminal_op, true);
            return;
        }

        auto targets = catalog->reset_targets(entry->id, intent.target_control_generation);
        if (!targets.ok()) {
            return;
        }
        for (const auto target : targets.value()) {
            (void)catalog->block_reads(target);
        }
        const auto timeout = owner->options->execution ? owner->options->execution->native_timeout
                                                       : owner->options->request_wait;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        Status failure;
        for (const auto target : targets.value()) {
            auto deleted = catalog->delete_artifact(
                target, intent.reset_generation, artifact::DeleteReason::Reset,
                task::control_wall_now(), deadline);
            if (!deleted.ok() && deleted.status().code() != StatusCode::NotFound) {
                failure = deleted.status();
                break;
            }
        }
        if (failure.ok()) {
            auto purged =
                catalog->purge_quarantined_for_reset(entry->id, intent.target_control_generation);
            if (!purged.ok()) {
                failure = purged.status();
            }
        }
        if (!failure.ok()) {
            // Transient lease timeout or I/O failure retains reset_intent so
            // subsequent observe() or boot reconciliation can finish Reset.
            if (failure.code() == StatusCode::DataLoss) {
                auto terminal_op = op->snapshot;
                terminal_op.state = OperationState::Failed;
                terminal_op.terminal_at =
                    std::max(task::control_wall_now(), terminal_op.accepted_at);
                terminal_op.status_code = StatusCode::DataLoss;
                terminal_op.failure_domain = FailureDomain::Volume;
                terminal_op.data_impact = DataImpact::HistoricalArtifactLostOrCorrupt;
                terminal_op.retryability = Retryability::ManualRecoveryRequired;
                (void)publish_terminal_sync(
                    owner->options->store.config, entry, terminal_op, false);
            }
            return;
        }

        auto progress =
            record_reset_progress(*entry, *control, intent, artifact::VolumeId{}, true, true, true);
        if (!progress.ok()) {
            return;
        }
        auto terminal_op = op->snapshot;
        terminal_op.state = OperationState::Succeeded;
        terminal_op.terminal_at = std::max(task::control_wall_now(), terminal_op.accepted_at);
        terminal_op.status_code = StatusCode::Ok;
        (void)publish_terminal_sync(owner->options->store.config, entry, terminal_op, true);
    } catch (...) {
        // Leave reset_intent durable for retry on next observe() or reboot.
    }
}

void TaskService::State::execute_reset(const std::shared_ptr<task::TaskEntry>& entry) noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(entry->control_mutex);
            if (entry->resetting || !entry->controller->durable_record()->reset_intent) {
                return;
            }
            entry->resetting = true;
            entry->quiescent = false;
        }
        auto job = std::make_shared<ResetJob>();
        job->owner = shared_from_this();
        job->entry = entry;
        if (options->execution && options->execution->cleanup_lane) {
            auto queued = options->execution->cleanup_lane->submit(job);
            if (!queued.ok()) {
                std::lock_guard<std::mutex> lock(entry->control_mutex);
                entry->resetting = false;
            }
        } else {
            job->run();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(entry->control_mutex);
        entry->resetting = false;
    }
}

void TaskService::State::execute_retire(
    const std::shared_ptr<task::TaskEntry>& entry, std::shared_ptr<task::Reply<OperationId>> reply,
    std::shared_ptr<const task::TaskControlRecord> accepted) noexcept {
    try {
        const auto* op = find_current_op(*accepted);
        if (!op || op->snapshot.kind != OperationKind::Retire) {
            reply->complete(Status(StatusCode::Internal, {}));
            return;
        }
        if (options->catalog) {
            auto eligible = options->catalog->retire_eligible(entry->id);
            if (!eligible.ok() || !eligible.value()) {
                auto failed = op->snapshot;
                failed.state = OperationState::Failed;
                failed.terminal_at = std::max(task::control_wall_now(), failed.accepted_at);
                failed.status_code = StatusCode::InvalidState;
                failed.retryability = Retryability::AfterConditionChange;
                (void)publish_terminal_sync(options->store.config, entry, failed, true);
                reply->complete(Status(StatusCode::InvalidState, {}));
                return;
            }
        }
        const auto now = std::max(task::control_wall_now(), op->snapshot.accepted_at);
        task::RetirementTombstone tombstone;
        tombstone.task_id = entry->id;
        tombstone.operation_id = op->snapshot.operation_id;
        tombstone.request_key = op->request.request_key;
        tombstone.request_digest = op->request_digest;
        tombstone.final_control_generation = accepted->control_generation;
        tombstone.accepted_at = op->snapshot.accepted_at;
        tombstone.retired_at = now;
        tombstone.cleanup_completed = false;

        auto published = retirements->publish(tombstone);
        if (!published.ok()) {
            reply->complete(published.status());
            return;
        }

        // Tombstone is now the irreversible linearization point: remove from active catalog.
        {
            std::lock_guard<std::mutex> lock(catalog_mutex);
            entries.erase(
                std::remove_if(
                    entries.begin(), entries.end(),
                    [&](const auto& item) { return item->id == entry->id; }),
                entries.end());
        }
        // Best-effort immediate post-retirement cleanup; any leftover is retried on boot/observe.
        if (cleanup_retired_task_files(entry->id).ok()) {
            (void)retirements->mark_cleanup_completed(entry->id);
        }
        reply->complete(op->snapshot.operation_id);
    } catch (const std::bad_alloc&) {
        reply->complete(Status(StatusCode::ResourceExhausted, {}));
    } catch (...) {
        reply->complete(Status(StatusCode::Internal, {}));
    }
}

Result<void> TaskService::State::cleanup_retired_task_files(TaskId task_id) {
    try {
        const auto id_str = to_string(task_id);
        std::error_code ec;
        fs::remove_all(options->root / "tasks" / id_str, ec);
        if (ec) {
            return Status(StatusCode::Unavailable, {});
        }
        for (const auto& file :
             {options->root / "control" / (id_str + ".json"),
              options->root / "identities" / (id_str + ".json"),
              options->root / "recovery" / (id_str + ".json")}) {
            ec.clear();
            fs::remove(file, ec);
            if (ec) {
                return Status(StatusCode::Unavailable, {});
            }
        }
        for (const auto* dir : {"tasks", "control", "identities", "recovery"}) {
            auto synced = persistence::sync_directory(
                options->root / dir, file_hook(task::ServiceFileDomain::Directories));
            if (!synced.ok()) {
                return synced.status();
            }
        }
        if (creates) {
            auto removed = creates->remove_for_retired_task(task_id);
            if (!removed.ok()) {
                return removed.status();
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (const fs::filesystem_error&) {
        return Status(StatusCode::Unavailable, {});
    }
}

Result<void> TaskService::reconcile_boot(artifact::ArtifactCatalog& catalog) {
    try {
        // 1. Finish any pending post-retirement cleanup before touching active tasks.
        if (state_->retirements) {
            for (const auto& tombstone : state_->retirements->all()) {
                if (!tombstone.cleanup_completed) {
                    if (state_->cleanup_retired_task_files(tombstone.task_id).ok()) {
                        (void)state_->retirements->mark_cleanup_completed(tombstone.task_id);
                    }
                }
            }
        }

        std::vector<std::shared_ptr<task::TaskEntry>> entries;
        {
            std::lock_guard<std::mutex> lock(state_->catalog_mutex);
            entries = state_->entries;
        }
        for (const auto& entry : entries) {
            task::BootAction action = task::BootAction::None;
            std::shared_ptr<const task::TaskControlRecord> control;
            {
                std::lock_guard<std::mutex> lock(entry->control_mutex);
                action = entry->boot_action;
                entry->boot_action = task::BootAction::None;
                control = entry->controller->durable_record();
            }
            if (action == task::BootAction::None) {
                continue;
            }
            if (action == task::BootAction::SettleInterruptedClose) {
                (void)end_stale_boot_run(*entry, *control, true);
                const auto* op = find_current_op(*control);
                if (op && !task::terminal(op->snapshot.state)) {
                    auto terminal_op = op->snapshot;
                    terminal_op.state = OperationState::Failed;
                    terminal_op.terminal_at =
                        std::max(task::control_wall_now(), terminal_op.accepted_at);
                    terminal_op.status_code = StatusCode::Unavailable;
                    terminal_op.failure_domain = FailureDomain::Process;
                    terminal_op.data_impact = DataImpact::PersistenceUnknown;
                    terminal_op.retryability = Retryability::AfterProcessRestart;
                    auto published = publish_terminal_sync(
                        state_->options->store.config, entry, terminal_op, true);
                    if (!published.ok()) {
                        return published.status();
                    }
                }
            } else if (action == task::BootAction::ResumeReset) {
                state_->execute_reset(entry);
            } else if (action == task::BootAction::ResumeRunOperation) {
                auto ended = end_stale_boot_run(*entry, *control, false);
                if (!ended.ok()) {
                    return ended.status();
                }
                {
                    std::lock_guard<std::mutex> lock(entry->control_mutex);
                    entry->controller->authorize_boot_run();
                    entry->quiescent = true;
                }
                state_->start_execution(entry, task::RunPurpose::Operation);
            } else if (action == task::BootAction::AutoResumeInterrupted) {
                auto ended = end_stale_boot_run(*entry, *control, false);
                if (!ended.ok()) {
                    return ended.status();
                }
                {
                    std::lock_guard<std::mutex> lock(entry->control_mutex);
                    entry->controller->authorize_boot_run();
                    entry->quiescent = true;
                    entry->volume_recovery =
                        task::VolumeRecovery{control->control_generation, 0, 0};
                }
                state_->start_execution(entry, task::RunPurpose::VolumeRecovery, 0);
            }
        }
        (void)catalog;
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> TaskService::collect_retired(
    artifact::ArtifactCatalog& catalog, std::chrono::nanoseconds audit_window) {
    if (!state_->retirements || !state_->accepting.load(std::memory_order_acquire)) {
        return {};
    }
    try {
        const auto now = task::control_wall_now();
        for (const auto& tombstone : state_->retirements->all()) {
            if (!tombstone.cleanup_completed) {
                if (!state_->cleanup_retired_task_files(tombstone.task_id).ok()) {
                    continue;
                }
                if (!state_->retirements->mark_cleanup_completed(tombstone.task_id).ok()) {
                    continue;
                }
            }
            if (!audit_window_elapsed(tombstone.retired_at, now, audit_window)) {
                continue;
            }
            auto referenced = catalog.has_task_references(tombstone.task_id);
            if (!referenced.ok() || referenced.value()) {
                continue;
            }
            (void)state_->retirements->collect(tombstone.task_id);
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq
