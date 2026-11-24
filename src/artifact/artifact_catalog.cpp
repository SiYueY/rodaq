#include "artifact_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "catalog_state.hpp"
#include "../pipeline/recording_segment.hpp"

namespace rodaq::artifact {

std::size_t CatalogState::find(std::uint64_t id) const noexcept {
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].segment_id == id) {
            return i;
        }
    }
    return slots.size();
}

Result<bool> CatalogState::public_entry(const CatalogSlot& slot) const {
    if (!slot.visible || !slot.entry) {
        return false;
    }
    auto authority = options.ledger->get(slot.segment_id);
    if (!authority.ok()) {
        return Status(
            authority.status().code() == StatusCode::NotFound ? StatusCode::DataLoss
                                                              : authority.status().code(),
            {});
    }
    return authority.value().state == LedgerState::Committed;
}

void CatalogState::corrupt(std::uint64_t id) noexcept {
    std::lock_guard<std::mutex> lock(index);
    const auto at = find(id);
    if (at != slots.size()) {
        slots[at].visible = false;
        slots[at].failure = StatusCode::DataLoss;
        if (slots[at].entry) {
            slots[at].entry->read_blocked = true;
        }
    }
}

ArtifactCatalog::ArtifactCatalog(std::shared_ptr<CatalogState> state) noexcept
: state_(std::move(state)) {}

Result<std::shared_ptr<ArtifactCatalog>> ArtifactCatalog::create(CatalogOptions options) {
    if (!options.volume || !options.ledger || !options.capacity ||
        !validate_manifest_limits(options.manifest).ok() ||
        options.max_artifacts < options.ledger->capacity() || options.max_artifacts > 65536 ||
        options.max_read_leases == 0 || options.max_read_leases > 4096 ||
        options.max_read_bytes == 0 || options.max_read_bytes > 16 * 1024 * 1024 ||
        options.read_lease_duration <= std::chrono::nanoseconds::zero() ||
        options.read_lease_duration > std::chrono::hours{24}) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto verified = options.volume->verify();
    if (!verified.ok()) {
        return verified.status();
    }
    try {
        auto state = std::make_shared<CatalogState>();
        state->slots.resize(options.max_artifacts);
        state->readers.resize(options.max_read_leases);
        state->options = std::move(options);
        return std::shared_ptr<ArtifactCatalog>(new ArtifactCatalog(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> ArtifactCatalog::commit(const pipeline::SealedPipelineSegment& sealed) {
    auto state = state_;
    if (!sealed.segment || sealed.work_id == 0) {
        return Status(StatusCode::InvalidArgument, {});
    }
    const auto id = sealed.segment->metadata.segment_id();
    std::size_t at = state->slots.size();
    std::shared_ptr<CommitFrame> frame;
    bool claimed = false;
    const auto finish = [&](StatusCode failure) -> Result<void> {
        std::lock_guard<std::mutex> lock(state->index);
        state->slots[at].work = CatalogWork::None;
        state->slots[at].failure = failure;
        return failure == StatusCode::Ok ? Result<void>{} : Result<void>(Status(failure, {}));
    };
    try {
        frame = std::make_shared<CommitFrame>(sealed);
        {
            std::lock_guard<std::mutex> lock(state->index);
            if (state->rebuilding) {
                return Status(StatusCode::Conflict, {});
            }
            at = state->find(id);
            if (at != state->slots.size()) {
                const auto& existing = state->slots[at];
                if (existing.work_id != sealed.work_id || existing.work != CatalogWork::None ||
                    existing.collection_pending) {
                    return Status(StatusCode::Conflict, {});
                }
                if (state->options.volume->protection() != StatusCode::Ok) {
                    return Status(state->options.volume->protection(), {});
                }
                auto published = state->public_entry(existing);
                if (!published.ok()) {
                    return published.status();
                }
                return published.value() ? Result<void>{}
                       : existing.failure != StatusCode::Ok
                           ? Result<void>(Status(existing.failure, {}))
                           : Result<void>(Status(StatusCode::InvalidState, {}));
            }
            const auto empty = std::find_if(
                state->slots.begin(), state->slots.end(),
                [](const auto& slot) { return slot.segment_id == 0; });
            if (empty == state->slots.end()) {
                return Status(StatusCode::ResourceExhausted, {});
            }
            at = static_cast<std::size_t>(empty - state->slots.begin());
            empty->segment_id = id;
            empty->work_id = sealed.work_id;
            empty->frame = frame;
            empty->work = CatalogWork::Commit;
            claimed = true;
        }
        const auto& input = frame->sealed;
        auto files = SealedFiles::open(
            state->options.volume, id, input.files, input.envelope,
            input.manifest_limits.max_bytes);
        if (!files.ok()) {
            return finish(files.status().code());
        }
        frame->files = std::move(files.value());
        auto manifest = input.segment->metadata.capture_manifest(frame->files->evidence());
        if (!manifest.ok()) {
            return finish(manifest.status().code());
        }
        auto reservation = input.segment->capacity.handoff_sealed(frame->files->storage_bytes());
        if (!reservation.ok()) {
            return finish(reservation.status().code());
        }
        frame->reservation.emplace(std::move(reservation.value()));
        auto committer = SegmentCommitter::create(
            frame->files, manifest.value(), input.manifest_limits, *frame->reservation);
        if (!committer.ok()) {
            return finish(committer.status().code());
        }
        frame->committer = std::move(committer.value());
        auto committed = frame->committer->commit(*state->options.ledger);
        if (!committed.ok()) {
            return finish(committed.status().code());
        }
        auto entry = verify_committed(state->options, frame->committer->ledger_record());
        if (!entry.ok()) {
            return finish(entry.status().code());
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            state->slots[at].entry = entry.value();
            state->slots[at].visible = true;
        }
        auto charge = frame->committer->take_committed_charge();
        if (!charge.ok()) {
            state->corrupt(id);
            return finish(charge.status().code());
        }
        {
            std::lock_guard<std::mutex> lock(state->index);
            entry.value()->charge.emplace(std::move(charge.value()));
            state->slots[at].frame.reset();
        }
        // Drop pinned Commit file descriptors outside the index lock. Retained
        // disk/metadata charge now belongs to the Catalog entry, not its cache age.
        frame->committer.reset();
        return finish(StatusCode::Ok);
    } catch (const std::bad_alloc&) {
        return claimed ? finish(StatusCode::ResourceExhausted)
                       : Result<void>(Status(StatusCode::ResourceExhausted, {}));
    } catch (...) {
        return claimed ? finish(StatusCode::Internal)
                       : Result<void>(Status(StatusCode::Internal, {}));
    }
}

Result<void> ArtifactCatalog::rebuild() {
    auto state = state_;
    {
        std::lock_guard<std::mutex> lock(state->index);
        if (state->rebuilding || std::any_of(
                                     state->slots.begin(), state->slots.end(),
                                     [](const auto& slot) { return slot.segment_id != 0; })) {
            return Status(StatusCode::InvalidState, {});
        }
        state->rebuilding = true;
    }
    struct Rebuilding {
        std::shared_ptr<CatalogState> state;
        ~Rebuilding() {
            std::lock_guard<std::mutex> lock(state->index);
            state->rebuilding = false;
        }
    } rebuilding{state};
    try {
        auto records = state->options.ledger->recovery_snapshot();
        if (!records.ok()) {
            return records.status();
        }
        StatusCode failure = StatusCode::Ok;
        std::size_t at = 0;
        for (const auto& record : records.value()) {
            if (record.state == LedgerState::Deleted) {
                if (!(record.volume_id == state->options.volume->identity())) {
                    return Status(StatusCode::DataLoss, {});
                }
                auto absent = prove_artifact_absence(state->options, record.segment_id);
                if (!absent.ok()) {
                    return absent.status();
                }
                auto entry = std::make_shared<CatalogEntry>();
                entry->info.summary.artifact_id = {record.segment_id};
                entry->info.summary.segment_id = record.segment_id;
                entry->info.summary.task_id = record.task_id;
                entry->info.summary.pipeline_id = record.pipeline_id;
                entry->read_blocked = true;
                auto charge = state->options.capacity->claim_recovered_artifact(0);
                if (!charge.ok()) {
                    return charge.status();
                }
                auto deleted = charge.value().release_after_delete();
                if (!deleted.ok()) {
                    return deleted.status();
                }
                entry->charge.emplace(std::move(charge.value()));
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.entry = std::move(entry);
                continue;
            }
            if (record.state != LedgerState::Committed) {
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.failure = StatusCode::InvalidState;
                if (failure == StatusCode::Ok) {
                    failure = StatusCode::InvalidState;
                }
                continue;
            }
            auto verified = verify_committed(state->options, record);
            if (!verified.ok()) {
                failure = verified.status().code();
                std::lock_guard<std::mutex> lock(state->index);
                auto& slot = state->slots[at++];
                slot.segment_id = record.segment_id;
                slot.failure = failure;
                continue;
            }
            auto charge = state->options.capacity->claim_recovered_artifact(
                verified.value()->info.summary.physical_bytes);
            if (!charge.ok()) {
                return charge.status();
            }
            verified.value()->charge.emplace(std::move(charge.value()));
            std::lock_guard<std::mutex> lock(state->index);
            auto& slot = state->slots[at++];
            slot.segment_id = record.segment_id;
            slot.entry = std::move(verified.value());
            slot.visible = true;
        }
        return failure == StatusCode::Ok ? Result<void>{} : Result<void>(Status(failure, {}));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

CatalogSnapshot ArtifactCatalog::snapshot() const {
    auto state = state_;
    CatalogSnapshot result;
    std::lock_guard<std::mutex> lock(state->index);
    result.protection = state->options.volume->protection() == StatusCode::Ok
                            ? state->options.ledger->protection()
                            : state->options.volume->protection();
    for (const auto& slot : state->slots) {
        auto public_fact = state->public_entry(slot);
        const bool normal = public_fact.ok() && public_fact.value();
        if (!public_fact.ok() && result.protection == StatusCode::Ok) {
            result.protection = public_fact.status().code();
        }
        if (slot.failure != StatusCode::Ok && result.protection == StatusCode::Ok) {
            result.protection = slot.failure;
        }
        result.visible += normal ? 1 : 0;
        bool deleted = slot.collection_pending;
        if (slot.segment_id != 0 && !normal) {
            auto record = state->options.ledger->get(slot.segment_id);
            deleted = deleted || (record.ok() && record.value().state == LedgerState::Deleted);
        }
        result.pending += slot.segment_id != 0 && !normal && !deleted ? 1 : 0;
        result.commit_inflight += slot.work == CatalogWork::Commit ? 1 : 0;
        result.delete_inflight += slot.work == CatalogWork::Delete ? 1 : 0;
        result.collect_inflight += slot.work == CatalogWork::Collect ? 1 : 0;
    }
    result.read_leases = state->read_leases;
    result.reads_inflight = state->active_reads.load();
    return result;
}

}  // namespace rodaq::artifact
