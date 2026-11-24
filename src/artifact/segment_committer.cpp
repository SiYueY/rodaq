#include "segment_committer.hpp"

#include <algorithm>
#include <chrono>
#include <new>
#include <limits>
#include <utility>

namespace rodaq::artifact {

Result<void> SegmentCommitter::recover(
    std::shared_ptr<ManagedVolume> volume, LedgerStore& ledger, std::uint64_t segment_id,
    const ManifestLimits& limits,
    const std::function<Result<void>(const LedgerRecord&, const SegmentManifest&)>& authorize,
    persistence::FileFaultHook faults) {
    if (!authorize) {
        return Status(StatusCode::InvalidArgument, {});
    }
    try {
        auto record = ledger.get(segment_id);
        if (!record.ok()) {
            return record.status();
        }
        if (record.value().state != LedgerState::Committing) {
            return Status(StatusCode::InvalidState, {});
        }
        auto files =
            SealedFiles::recover(std::move(volume), record.value(), limits, std::move(faults));
        if (!files.ok()) {
            return files.status();
        }
        auto allowed = authorize(record.value(), files.value()->recovery_manifest());
        if (!allowed.ok()) {
            return allowed.status();
        }
        auto published = files.value()->finish_recovery_publication();
        if (!published.ok()) {
            return published.status();
        }
        allowed = authorize(record.value(), files.value()->recovery_manifest());
        if (!allowed.ok()) {
            return allowed.status();
        }
        auto valid = files.value()->verify_recovery_publication();
        if (!valid.ok()) {
            return valid.status();
        }
        auto candidate = std::move(record.value());
        candidate.state = LedgerState::Committed;
        candidate.committed_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        return ledger.advance(candidate, LedgerState::Committing);
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    } catch (...) {
        return Status(StatusCode::Internal, {});
    }
}

SegmentCommitter::SegmentCommitter(
    std::unique_ptr<SealedFiles> files, std::string encoded, LedgerRecord record,
    CommitReservation reservation) noexcept
: files_(std::move(files)),
  encoded_manifest_(std::move(encoded)),
  record_(std::move(record)),
  reservation_(std::move(reservation)) {}

Result<std::unique_ptr<SegmentCommitter>> SegmentCommitter::create(
    std::unique_ptr<SealedFiles>& files, const SegmentManifest& manifest,
    const ManifestLimits& limits, CommitReservation& reservation) {
    if (!files || reservation.registration.segment_id() != manifest.identity.segment_id ||
        files->segment_id() != manifest.identity.segment_id ||
        !(files->volume_id() == manifest.identity.volume_id)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto valid = validate_manifest(manifest, limits);
    if (!valid.ok()) {
        return valid.status();
    }
    // Unknown writes cannot become normal committed artifacts. A Segment with
    // no written record is not an artifact merely because its file set seals.
    if (std::any_of(
            manifest.coverage.begin(), manifest.coverage.end(),
            [](const auto& item) { return item.unknown != 0; }) ||
        std::none_of(manifest.coverage.begin(), manifest.coverage.end(), [](const auto& item) {
            return item.written != 0;
        })) {
        return Status(StatusCode::DataLoss, {});
    }
    if (manifest.files.size() != files->evidence().size()) {
        return Status(StatusCode::DataLoss, {});
    }
    for (const auto& expected : files->evidence()) {
        const auto actual = std::find_if(
            manifest.files.begin(), manifest.files.end(),
            [&](const auto& entry) { return entry.relative_path == expected.relative_path; });
        if (actual == manifest.files.end() || actual->physical_size != expected.physical_size ||
            actual->digest != expected.digest) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    auto encoded = encode_manifest(manifest, limits);
    if (!encoded.ok()) {
        return encoded.status();
    }
    if (reservation.manifest_bound == 0 ||
        reservation.storage_bytes >
            std::numeric_limits<std::uint64_t>::max() - reservation.manifest_bound ||
        reservation.storage_bytes != files->storage_bytes() ||
        encoded.value().size() > reservation.manifest_bound) {
        return Status(StatusCode::DataLoss, {});
    }
    auto digest = manifest_digest(manifest, limits);
    if (!digest.ok()) {
        return digest.status();
    }
    try {
        LedgerRecord record;
        record.segment_id = manifest.identity.segment_id;
        record.task_id = manifest.identity.task_id;
        record.pipeline_id = manifest.identity.pipeline_id;
        record.volume_id = manifest.identity.volume_id;
        record.managed_relative_path = ".rodaq/artifacts/" + std::to_string(record.segment_id);
        record.manifest_digest = std::move(digest.value());
        record.control_generation = manifest.identity.control_generation;
        record.reset_generation_at_commit = manifest.identity.reset_generation_at_commit;
        return std::unique_ptr<SegmentCommitter>(new SegmentCommitter(
            std::move(files), std::move(encoded.value()), std::move(record),
            std::move(reservation)));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> SegmentCommitter::commit(LedgerStore& ledger) {
    if (attempted_) {
        return Status(StatusCode::InvalidState, {});
    }
    attempted_ = true;
    reservation_.volume.mark_effects_possible();
    auto installed = files_->install_manifest(encoded_manifest_);
    if (!installed.ok()) {
        return installed.status();
    }
    auto accounted =
        reservation_.volume.record_usage(reservation_.storage_bytes + encoded_manifest_.size(), 0);
    if (!accounted.ok()) {
        return accounted.status();
    }
    auto synced = files_->sync_tree();
    if (!synced.ok()) {
        return synced.status();
    }
    record_.commit_started_at =
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now());
    auto registered = ledger.create_committing(reservation_.registration, record_);
    if (!registered.ok()) {
        return registered.status();
    }
    auto published = files_->publish_directory();
    if (!published.ok()) {
        return published.status();
    }
    // Allocate/copy the candidate before advancing durable authority. Keep the
    // known committing projection on any non-OK outcome; recovery reconciles
    // uncertain Ledger publication instead of treating it as absence/success.
    try {
        auto candidate = record_;
        candidate.state = LedgerState::Committed;
        candidate.committed_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        auto completed = ledger.advance(candidate, LedgerState::Committing);
        if (!completed.ok()) {
            return completed.status();
        }
        record_ = std::move(candidate);
        committed_ = true;
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<ArtifactCharge> SegmentCommitter::take_committed_charge() {
    if (!committed_) {
        return Status(StatusCode::InvalidState, {});
    }
    return reservation_.volume.settle_retained();
}

}  // namespace rodaq::artifact
