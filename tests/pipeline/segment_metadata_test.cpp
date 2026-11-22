#include "pipeline/segment_metadata.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <gtest/gtest.h>

#include <future>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace {

namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;
using pipeline::RecordSettlementKind;
using rodaq::StatusCode;

rodaq::storage::PrepareConfig envelope() {
    rodaq::storage::PrepareConfig result{};
    result.max_segment_records = 1000;
    result.max_segment_file_count = 2;
    result.max_relative_path_bytes = 64;
    return result;
}

artifact::ManifestLimits limits() {
    artifact::ManifestLimits result;
    result.max_bytes = 8192;
    result.max_descriptors = 4;
    result.max_streams = 2;
    result.max_epochs = 2;
    return result;
}

rodaq::time::ClockSnapshot received(std::uint64_t mono = 100) {
    return {
        42,
        1,
        {mono, static_cast<std::int64_t>(mono + 1000), 1},
        rodaq::time::ClockEpochOrigin::Boot,
        {}};
}

std::vector<artifact::FileEvidence> files() {
    return {
        {"data.bin", 3, "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}};
}

void prepare(
    pipeline::SegmentMetadata& metadata, rodaq::StreamId stream = 1,
    rodaq_descriptor_id_t descriptor = 1) {
    auto reserved =
        metadata.reserve_descriptor(rodaq::test::manifest_descriptor(stream, descriptor));
    ASSERT_TRUE(reserved.ok());
    reserved.value().transfer();
    ASSERT_TRUE(reserved.value().mark_prepared().ok());
}

TEST(SegmentMetadata, DescriptorAndRecordRollbackReleaseOnlyTheirOwnPreallocatedMetadataCapacity) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    const auto baseline = metadata.snapshot().manifest_bytes_bound;
    {
        auto descriptor = metadata.reserve_descriptor(rodaq::test::manifest_descriptor());
        ASSERT_TRUE(descriptor.ok());
        EXPECT_EQ(descriptor.value().mark_prepared().status().code(), StatusCode::InvalidState);
        EXPECT_EQ(metadata.snapshot().pending_descriptors, 1);
        EXPECT_EQ(
            metadata.reserve_record(1, 1, received(), 0).status().code(), StatusCode::InvalidState);
    }
    EXPECT_EQ(metadata.snapshot().manifest_bytes_bound, baseline);
    EXPECT_EQ(metadata.snapshot().descriptors, 0);
    prepare(metadata);
    const auto prepared_bound = metadata.snapshot().manifest_bytes_bound;
    {
        auto candidate = metadata.reserve_record(1, 1, received(), 0);
        ASSERT_TRUE(candidate.ok());
        EXPECT_EQ(metadata.snapshot().pending_admissions, 1);
        EXPECT_EQ(metadata.snapshot().epochs, 1);
    }
    EXPECT_EQ(metadata.snapshot().pending_admissions, 0);
    EXPECT_EQ(metadata.snapshot().epochs, 0);
    EXPECT_EQ(metadata.snapshot().accepted, 0);
    EXPECT_EQ(metadata.snapshot().manifest_bytes_bound, prepared_bound);
    EXPECT_GT(metadata.snapshot().high_water_manifest_bytes_bound, prepared_bound);
}

TEST(SegmentMetadata, AdmissionReservationsAndAcceptedOutstandingWorkHaveDifferentLifetimes) {
    auto metadata = pipeline::SegmentMetadata::create(
                        rodaq::test::manifest_identity(), {1}, envelope(), limits())
                        .value();
    prepare(metadata);
    auto candidate = metadata.reserve_record(1, 1, received(), 0);
    ASSERT_TRUE(candidate.ok());
    EXPECT_EQ(metadata.snapshot().pending_admissions, 1);
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 0);
    auto accepted = candidate.value().accept(1);
    EXPECT_EQ(metadata.snapshot().pending_admissions, 0);
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 1);
    ASSERT_TRUE(accepted.mark_native_entered().ok());
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 1);
    ASSERT_TRUE(accepted.finish(pipeline::RecordSettlementKind::Written, 8).ok());
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 0);
}

TEST(SegmentMetadata, OutstandingCountRejectsImpossibleCountersWithoutUnsignedWrap) {
    pipeline::SegmentMetadataSnapshot snapshot;
    snapshot.accepted = std::numeric_limits<std::uint64_t>::max();
    snapshot.written = snapshot.accepted - 1;
    snapshot.unknown = 1;
    EXPECT_EQ(pipeline::unsettled_record_count(snapshot).value(), 0);
    snapshot.unknown = 2;
    EXPECT_EQ(pipeline::unsettled_record_count(snapshot).status().code(), StatusCode::DataLoss);
}

TEST(SegmentMetadata, ExactOnceSettlementBuildsTheAcceptedDescriptorEpochAndRecordingRanges) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    prepare(metadata);
    auto first = metadata.reserve_record(1, 1, received(100), 0);
    auto second = metadata.reserve_record(1, 1, received(200), 100);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    auto written = first.value().accept(10);
    auto failed = second.value().accept(11);
    EXPECT_EQ(metadata.capture_manifest(files()).status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(written.mark_native_entered().ok());
    EXPECT_EQ(written.mark_native_entered().status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(written.finish(RecordSettlementKind::Written, 17).ok());
    EXPECT_EQ(
        written.finish(RecordSettlementKind::Written, 0).status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(failed.finish(RecordSettlementKind::KnownFailed, 0).ok());
    metadata.close_admission();
    auto captured = metadata.capture_manifest(files());
    ASSERT_TRUE(captured.ok());
    EXPECT_EQ(captured.value().logical_bytes, 17);
    EXPECT_EQ(captured.value().coverage[0].accepted, 2);
    EXPECT_EQ(captured.value().coverage[0].written, 1);
    EXPECT_EQ(captured.value().coverage[0].known_failed, 1);
    EXPECT_EQ(captured.value().coverage[0].first_sequence, 10);
    EXPECT_EQ(captured.value().coverage[0].last_sequence, 11);
    EXPECT_EQ(captured.value().descriptors[0].accepted_records, 2);
    EXPECT_EQ(captured.value().epochs[0].accepted_records, 2);
    EXPECT_EQ(captured.value().first_receive_monotonic, 100);
    EXPECT_EQ(captured.value().last_receive_monotonic, 200);
    EXPECT_EQ(captured.value().recording_end_ns, 100);
    auto encoded = artifact::encode_manifest(captured.value(), limits());
    ASSERT_TRUE(encoded.ok());
    EXPECT_LE(encoded.value().size(), metadata.snapshot().manifest_bytes_bound);
}

TEST(SegmentMetadata, UnenteredRecordCannotBeWrittenOrUnknownAndPendingNativeReturnBlocksCapture) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    prepare(metadata);
    auto candidate = metadata.reserve_record(1, 1, received(), 0);
    ASSERT_TRUE(candidate.ok());
    auto claim = candidate.value().accept(1);
    EXPECT_EQ(
        claim.finish(RecordSettlementKind::Written, 0).status().code(), StatusCode::InvalidState);
    EXPECT_EQ(
        claim.finish(RecordSettlementKind::Unknown, 0).status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(claim.mark_native_entered().ok());
    metadata.close_admission();
    EXPECT_EQ(metadata.snapshot().native_inflight, 1);
    EXPECT_EQ(metadata.capture_manifest(files()).status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(claim.finish(RecordSettlementKind::Unknown, 0).ok());
    EXPECT_EQ(metadata.snapshot().native_inflight, 0);
    auto captured = metadata.capture_manifest(files());
    ASSERT_TRUE(captured.ok());
    EXPECT_EQ(captured.value().coverage[0].unknown, 1);
}

TEST(SegmentMetadata, DroppedUnfinishedOwnersNeverInventClassificationOrActualNativeReturn) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    prepare(metadata);
    {
        auto first = metadata.reserve_record(1, 1, received(), 0);
        ASSERT_TRUE(first.ok());
        auto unentered = first.value().accept(1);
        auto second = metadata.reserve_record(1, 1, received(101), 1);
        ASSERT_TRUE(second.ok());
        auto entered = second.value().accept(2);
        ASSERT_TRUE(entered.mark_native_entered().ok());
    }
    metadata.close_admission();
    const auto snapshot = metadata.snapshot();
    EXPECT_EQ(snapshot.accepted, 2);
    EXPECT_EQ(snapshot.unresolved_owners, 2);
    EXPECT_EQ(snapshot.native_inflight, 1);
    EXPECT_EQ(snapshot.known_failed, 0);
    EXPECT_EQ(snapshot.unknown, 0);
    EXPECT_EQ(metadata.capture_manifest(files()).status().code(), StatusCode::InvalidState);
}

TEST(SegmentMetadata, ByteAndEpochLimitsRejectBeforeAcceptAndCancelledEpochCanBeReusedAsCapacity) {
    auto bound_limits = limits();
    const auto identity = rodaq::test::manifest_identity();
    const auto descriptor = rodaq::test::manifest_descriptor();
    auto base = artifact::manifest_base_size_bound(identity, bound_limits);
    auto descriptor_bound = artifact::manifest_descriptor_size_bound(descriptor, bound_limits);
    ASSERT_TRUE(base.ok());
    ASSERT_TRUE(descriptor_bound.ok());
    bound_limits.max_bytes = base.value() + 2 * artifact::manifest_file_size_bound(64) +
                             artifact::kManifestStreamSizeBound + descriptor_bound.value() + 1 +
                             artifact::kManifestEpochSizeBound - 1;
    auto tight = pipeline::SegmentMetadata::create(identity, {1}, envelope(), bound_limits);
    ASSERT_TRUE(tight.ok());
    prepare(tight.value());
    EXPECT_EQ(
        tight.value().reserve_record(1, 1, received(), 0).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(tight.value().snapshot().accepted, 0);
    EXPECT_EQ(tight.value().snapshot().epochs, 0);
    bound_limits = limits();
    bound_limits.max_epochs = 1;
    auto created = pipeline::SegmentMetadata::create(identity, {1}, envelope(), bound_limits);
    ASSERT_TRUE(created.ok());
    prepare(created.value());
    {
        auto cancelled = created.value().reserve_record(1, 1, received(), 0);
        ASSERT_TRUE(cancelled.ok());
    }
    auto changed = received(200);
    changed.epoch_id = 2;
    changed.origin = rodaq::time::ClockEpochOrigin::RealtimeJump;
    changed.jump = rodaq::time::ClockJump{1, 500, 150};
    auto candidate = created.value().reserve_record(1, 1, changed, 100);
    ASSERT_TRUE(candidate.ok());
    auto claim = candidate.value().accept(1);
    ASSERT_TRUE(claim.finish(RecordSettlementKind::KnownFailed, 0).ok());
    EXPECT_EQ(
        created.value().reserve_record(1, 1, received(), 0).status().code(),
        StatusCode::ResourceExhausted);
    created.value().close_admission();
    auto captured = created.value().capture_manifest(files());
    ASSERT_TRUE(captured.ok());
    EXPECT_EQ(captured.value().epochs[0].epoch_id, 2);
}

TEST(SegmentMetadata, TransferredFailedDescriptorRetainsBoundAndUnusedReadyFactsStayOutOfManifest) {
    auto bound_limits = limits();
    bound_limits.max_descriptors = 1;
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), bound_limits);
    ASSERT_TRUE(created.ok());
    {
        auto descriptor = created.value().reserve_descriptor(rodaq::test::manifest_descriptor());
        ASSERT_TRUE(descriptor.ok());
        descriptor.value().transfer();
    }
    EXPECT_EQ(created.value().snapshot().descriptors, 1);
    EXPECT_EQ(created.value().snapshot().pending_descriptors, 0);
    EXPECT_EQ(
        created.value().reserve_descriptor(rodaq::test::manifest_descriptor(1, 2)).status().code(),
        StatusCode::ResourceExhausted);
    auto ready = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1, 2}, envelope(), limits());
    ASSERT_TRUE(ready.ok());
    prepare(ready.value(), 1, 1);
    prepare(ready.value(), 2, 2);
    auto candidate = ready.value().reserve_record(1, 1, received(), 0);
    ASSERT_TRUE(candidate.ok());
    auto claim = candidate.value().accept(1);
    ASSERT_TRUE(claim.finish(RecordSettlementKind::KnownFailed, 0).ok());
    ready.value().close_admission();
    auto captured = ready.value().capture_manifest(files());
    ASSERT_TRUE(captured.ok());
    EXPECT_EQ(captured.value().descriptors.size(), 1);
    EXPECT_EQ(captured.value().coverage.size(), 1);
}

TEST(SegmentMetadata, ConcurrentStreamOwnersKeepExactCountsAndNoUnresolvedOrPendingReservations) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1, 2}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    prepare(metadata, 1, 1);
    prepare(metadata, 2, 2);
    const auto work = [&](rodaq::StreamId stream) {
        for (std::uint64_t index = 0; index < 100; ++index) {
            auto reserved = metadata.reserve_record(stream, stream, received(100 + index), index);
            if (!reserved.ok()) {
                return false;
            }
            auto claim = reserved.value().accept(index + 1);
            if (!claim.mark_native_entered().ok() ||
                !claim.finish(RecordSettlementKind::Written, 0).ok()) {
                return false;
            }
        }
        return true;
    };
    auto first = std::async(std::launch::async, [&] { return work(1); });
    auto second = std::async(std::launch::async, [&] { return work(2); });
    EXPECT_TRUE(first.get());
    EXPECT_TRUE(second.get());
    metadata.close_admission();
    const auto snapshot = metadata.snapshot();
    EXPECT_EQ(snapshot.accepted, 200);
    EXPECT_EQ(snapshot.written, 200);
    EXPECT_EQ(snapshot.pending_admissions, 0);
    EXPECT_EQ(snapshot.native_inflight, 0);
    EXPECT_EQ(snapshot.unresolved_owners, 0);
    EXPECT_TRUE(metadata.capture_manifest(files()).ok());
}

TEST(SegmentMetadata, ConservativeBoundsCoverControlEscapesAndFullWidthNumericEncoding) {
    auto value = rodaq::test::manifest_identity();
    value.qualified_runtime_profile_id = std::string(256, '\1');
    auto descriptor = rodaq::test::manifest_descriptor();
    descriptor.channel_name = std::string(1024, '\2');
    descriptor.schema_name = std::string(1024, '\3');
    auto bound_limits = limits();
    bound_limits.max_bytes = 65536;
    auto created = pipeline::SegmentMetadata::create(value, {1}, envelope(), bound_limits);
    ASSERT_TRUE(created.ok());
    auto reserved = created.value().reserve_descriptor(std::move(descriptor));
    ASSERT_TRUE(reserved.ok());
    reserved.value().transfer();
    ASSERT_TRUE(reserved.value().mark_prepared().ok());
    auto clock = received();
    clock.sample.error_bound_ns = std::numeric_limits<std::uint64_t>::max();
    auto candidate =
        created.value().reserve_record(1, 1, clock, std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(candidate.ok());
    auto claim = candidate.value().accept(std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(claim.finish(RecordSettlementKind::KnownFailed, 0).ok());
    created.value().close_admission();
    auto proof = files();
    proof[0].relative_path = std::string(64, '\4');
    auto captured = created.value().capture_manifest(std::move(proof));
    ASSERT_TRUE(captured.ok());
    const auto encoded = artifact::encode_manifest(captured.value(), bound_limits);
    ASSERT_TRUE(encoded.ok());
    EXPECT_LE(encoded.value().size(), created.value().snapshot().manifest_bytes_bound);
}

TEST(SegmentMetadata, ClosingAndObserverExpirationCannotFinishTheActualInflightClaim) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    prepare(metadata);
    auto candidate = metadata.reserve_record(1, 1, received(), 0);
    ASSERT_TRUE(candidate.ok());
    auto claim = candidate.value().accept(1);
    std::promise<void> entered;
    std::promise<void> release;
    auto allowed = release.get_future().share();
    auto actual =
        std::async(std::launch::async, [owned = std::move(claim), &entered, allowed]() mutable {
            const bool marked = owned.mark_native_entered().ok();
            entered.set_value();
            allowed.wait();
            return marked && owned.finish(RecordSettlementKind::Written, 0).ok();
        });
    entered.get_future().wait();
    metadata.close_admission();
    EXPECT_EQ(actual.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);
    EXPECT_EQ(metadata.snapshot().native_inflight, 1);
    EXPECT_EQ(metadata.snapshot().written, 0);
    EXPECT_EQ(metadata.capture_manifest(files()).status().code(), StatusCode::InvalidState);
    release.set_value();
    EXPECT_TRUE(actual.get());
    EXPECT_EQ(metadata.snapshot().native_inflight, 0);
    EXPECT_EQ(metadata.snapshot().written, 1);
    EXPECT_TRUE(metadata.capture_manifest(files()).ok());
}

}  // namespace
