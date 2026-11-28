#include "segment_committer.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace rodaq::artifact {

SegmentCommitter::SegmentCommitter(
    std::unique_ptr<SealedFiles> files, std::string encoded, CommitReservation reservation,
    DurabilityProfile durability) noexcept
: files_(std::move(files)),
  encoded_manifest_(std::move(encoded)),
  reservation_(std::move(reservation)),
  durability_(durability) {}

Result<std::unique_ptr<SegmentCommitter>> SegmentCommitter::create(
    std::unique_ptr<SealedFiles>& files, const SegmentManifest& manifest,
    const ManifestLimits& limits, CommitReservation& reservation, DurabilityProfile durability) {
    if (!files || files->segment_id() != manifest.identity.segment_id ||
        !(files->volume_id() == manifest.identity.volume_id)) {
        return Status(StatusCode::InvalidArgument, {});
    }
    auto valid = validate_manifest(manifest, limits);
    if (!valid.ok()) {
        return valid.status();
    }
    // Unknown writes cannot become normal committed artifacts, and a Segment
    // with zero written records is not an artifact.
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
        if (actual == manifest.files.end() || actual->physical_size != expected.physical_size) {
            return Status(StatusCode::DataLoss, {});
        }
    }
    auto encoded = encode_manifest(manifest, limits);
    if (!encoded.ok()) {
        return encoded.status();
    }
    if (reservation.storage_bytes != files->storage_bytes() ||
        encoded.value().size() >
            std::numeric_limits<std::uint64_t>::max() - reservation.storage_bytes) {
        return Status(StatusCode::DataLoss, {});
    }
    auto accounted = reservation.volume.record_sealed_usage(files->storage_bytes());
    if (!accounted.ok()) {
        return accounted.status();
    }
    try {
        return std::unique_ptr<SegmentCommitter>(new SegmentCommitter(
            std::move(files), std::move(encoded.value()), std::move(reservation), durability));
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

Result<void> SegmentCommitter::commit() {
    if (attempted_) {
        return Status(StatusCode::InvalidState, {});
    }
    attempted_ = true;
    const bool hardened = durability_ == DurabilityProfile::PowerLossHardened;
    reservation_.volume.mark_effects_possible();
    auto installed = files_->install_manifest(encoded_manifest_, hardened);
    if (!installed.ok()) {
        return installed.status();
    }
    auto accounted =
        reservation_.volume.record_usage(reservation_.storage_bytes + encoded_manifest_.size(), 0);
    if (!accounted.ok()) {
        return accounted.status();
    }
    if (hardened) {
        auto synced = files_->sync_tree();
        if (!synced.ok()) {
            return synced.status();
        }
    }
    auto published = files_->publish_directory(hardened);
    if (!published.ok()) {
        return published.status();
    }
    committed_ = true;
    return {};
}

Result<ArtifactCharge> SegmentCommitter::take_committed_charge() {
    if (!committed_) {
        return Status(StatusCode::InvalidState, {});
    }
    return reservation_.volume.settle_retained();
}

Result<void> SegmentCommitter::abort_after_cleanup() {
    if (committed_) {
        return Status(StatusCode::InvalidState, {});
    }
    return reservation_.volume.settle_aborted_after_cleanup();
}

}  // namespace rodaq::artifact
