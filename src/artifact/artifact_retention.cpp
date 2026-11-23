#include "catalog_state.hpp"

#include <algorithm>
#include <limits>
#include <new>

#include "../persistence/json.hpp"
#include "../uuid.hpp"

namespace rodaq::artifact {

Result<RetentionSelection> ArtifactCatalog::select_retention(
    TaskId task, const RetentionPolicy& policy,
    const std::vector<std::string>& eligible_config_versions) const {
    if (!uuid::nonzero(task.value) ||
        eligible_config_versions.size() > state_->options.max_artifacts ||
        (policy.max_duration && policy.max_duration->count() <= 0) ||
        (policy.max_physical_bytes && *policy.max_physical_bytes == 0) ||
        std::any_of(
            eligible_config_versions.begin(), eligible_config_versions.end(),
            [](const auto& version) { return !persistence::valid_digest(version); })) {
        return Status(StatusCode::InvalidArgument, {});
    }
    if (policy.mode == RetentionMode::KeepAll) {
        return !policy.max_duration && !policy.max_physical_bytes &&
                       !policy.include_previous_continuous
                   ? Result<RetentionSelection>(RetentionSelection{})
                   : Result<RetentionSelection>(Status(StatusCode::InvalidArgument, {}));
    }
    if (policy.mode != RetentionMode::Rolling ||
        (!policy.max_duration && !policy.max_physical_bytes)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        RetentionSelection result;
        std::vector<std::shared_ptr<CatalogEntry>> selected;
        selected.reserve(state_->slots.size());
        std::lock_guard<std::mutex> lock(state_->index);
        if (state_->options.volume->protection() != StatusCode::Ok) {
            return Status(state_->options.volume->protection(), {});
        }
        if (state_->options.ledger->protection() != StatusCode::Ok) {
            return Status(state_->options.ledger->protection(), {});
        }
        for (const auto& slot : state_->slots) {
            if (!slot.entry || !(slot.entry->info.summary.task_id == task)) {
                continue;
            }
            auto visible = state_->public_entry(slot);
            if (!visible.ok()) {
                return visible.status();
            }
            if (!visible.value()) {
                continue;
            }
            const auto& summary = slot.entry->info.summary;
            result.committed_recording_end_ns =
                std::max(result.committed_recording_end_ns, summary.recording_range.end_ns);
            if (std::find(
                    eligible_config_versions.begin(), eligible_config_versions.end(),
                    slot.entry->config_version) == eligible_config_versions.end()) {
                continue;
            }
            if (summary.physical_bytes >
                std::numeric_limits<std::uint64_t>::max() - result.retained_physical_bytes) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            result.retained_physical_bytes += summary.physical_bytes;
            if (slot.work == CatalogWork::None && !slot.frame && slot.entry->readers == 0 &&
                !slot.entry->read_blocked) {
                selected.push_back(slot.entry);
            }
        }
        result.byte_target_satisfied = !policy.max_physical_bytes ||
                                       result.retained_physical_bytes <= *policy.max_physical_bytes;
        const auto duration =
            policy.max_duration ? static_cast<std::uint64_t>(policy.max_duration->count()) : 0;
        const auto left = result.committed_recording_end_ns > duration
                              ? result.committed_recording_end_ns - duration
                              : 0;
        const CatalogEntry* oldest = nullptr;
        for (const auto& entry : selected) {
            const auto& summary = entry->info.summary;
            // Duration protects every artifact touching/crossing the left edge.
            // An unmet byte target never authorizes deleting inside that window.
            if ((policy.max_duration && summary.recording_range.end_ns >= left) ||
                (!policy.max_duration && result.byte_target_satisfied)) {
                continue;
            }
            if (!oldest ||
                summary.recording_range.end_ns < oldest->info.summary.recording_range.end_ns ||
                (summary.recording_range.end_ns == oldest->info.summary.recording_range.end_ns &&
                 summary.segment_id < oldest->info.summary.segment_id)) {
                oldest = entry.get();
            }
        }
        if (oldest) {
            result.candidate = oldest->info.summary.artifact_id;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::artifact
