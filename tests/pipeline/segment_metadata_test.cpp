#include "pipeline/segment_metadata.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <vector>

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
    return result;
}

rodaq::time::ClockSample received(std::uint64_t mono = 100) {
    return {mono, static_cast<std::int64_t>(mono + 1000), 1};
}

std::vector<artifact::FileEvidence> files() { return {{"data.mcap", 3}}; }

rodaq::WallTime committed_at() {
    return rodaq::WallTime{std::chrono::nanoseconds{1700000000000000000LL}};
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

TEST(SegmentMetadata, DescriptorAndRecordRollbackReleaseReservationsCleanly) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    {
        auto descriptor = metadata.reserve_descriptor(rodaq::test::manifest_descriptor());
        ASSERT_TRUE(descriptor.ok());
        EXPECT_EQ(descriptor.value().mark_prepared().status().code(), StatusCode::InvalidState);
        EXPECT_EQ(metadata.snapshot().pending_descriptors, 1U);
        EXPECT_EQ(
            metadata.reserve_record(1, 1, received(), 0).status().code(), StatusCode::InvalidState);
    }
    EXPECT_EQ(metadata.snapshot().descriptors, 0U);
    prepare(metadata);
    {
        auto candidate = metadata.reserve_record(1, 1, received(), 0);
        ASSERT_TRUE(candidate.ok());
        EXPECT_EQ(metadata.snapshot().pending_admissions, 1U);
    }
    EXPECT_EQ(metadata.snapshot().pending_admissions, 0U);
    EXPECT_EQ(metadata.snapshot().accepted, 0ULL);
}

TEST(SegmentMetadata, AdmissionReservationsAndAcceptedOutstandingWorkHaveDifferentLifetimes) {
    auto metadata = pipeline::SegmentMetadata::create(
                        rodaq::test::manifest_identity(), {1}, envelope(), limits())
                        .value();
    prepare(metadata);
    auto candidate = metadata.reserve_record(1, 1, received(), 0);
    ASSERT_TRUE(candidate.ok());
    EXPECT_EQ(metadata.snapshot().pending_admissions, 1U);
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 0ULL);
    auto accepted = candidate.value().accept(1);
    EXPECT_EQ(metadata.snapshot().pending_admissions, 0U);
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 1ULL);
    ASSERT_TRUE(accepted.mark_native_entered().ok());
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 1ULL);
    ASSERT_TRUE(accepted.finish(RecordSettlementKind::Written, 8).ok());
    EXPECT_EQ(pipeline::unsettled_record_count(metadata.snapshot()).value(), 0ULL);
}

TEST(SegmentMetadata, OutstandingCountRejectsImpossibleCountersWithoutUnsignedWrap) {
    pipeline::SegmentMetadataSnapshot snapshot;
    snapshot.accepted = std::numeric_limits<std::uint64_t>::max();
    snapshot.written = snapshot.accepted - 1;
    snapshot.unknown = 1;
    EXPECT_EQ(pipeline::unsettled_record_count(snapshot).value(), 0ULL);
    snapshot.unknown = 2;
    EXPECT_EQ(pipeline::unsettled_record_count(snapshot).status().code(), StatusCode::DataLoss);
}

TEST(SegmentMetadata, ExactOnceSettlementBuildsTheAcceptedDescriptorAndRecordingRanges) {
    auto created = pipeline::SegmentMetadata::create(
        rodaq::test::manifest_identity(), {1}, envelope(), limits());
    ASSERT_TRUE(created.ok());
    auto& metadata = created.value();
    prepare(metadata);
    auto first = metadata.reserve_record(1, 1, received(100), 1000);
    auto second = metadata.reserve_record(1, 1, received(200), 1100);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    auto written = first.value().accept(10);
    auto failed = second.value().accept(11);
    EXPECT_EQ(
        metadata.capture_manifest(files(), committed_at()).status().code(),
        StatusCode::InvalidState);
    ASSERT_TRUE(written.mark_native_entered().ok());
    EXPECT_EQ(written.mark_native_entered().status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(written.finish(RecordSettlementKind::Written, 17).ok());
    EXPECT_EQ(
        written.finish(RecordSettlementKind::Written, 0).status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(failed.finish(RecordSettlementKind::KnownFailed, 0).ok());
    metadata.close_admission();
    auto captured = metadata.capture_manifest(files(), committed_at());
    ASSERT_TRUE(captured.ok()) << captured.status().message();
    EXPECT_EQ(captured.value().logical_bytes, 17ULL);
    EXPECT_EQ(captured.value().coverage[0].accepted, 2ULL);
    EXPECT_EQ(captured.value().coverage[0].written, 1ULL);
    EXPECT_EQ(captured.value().coverage[0].known_failed, 1ULL);
    EXPECT_EQ(captured.value().coverage[0].first_sequence, 10ULL);
    EXPECT_EQ(captured.value().coverage[0].last_sequence, 11ULL);
    EXPECT_EQ(captured.value().descriptors[0].accepted_records, 2ULL);
    EXPECT_EQ(captured.value().first_receive_monotonic, 100ULL);
    EXPECT_EQ(captured.value().last_receive_monotonic, 200ULL);
    EXPECT_EQ(captured.value().recording_begin_ns, 1000ULL);
    EXPECT_EQ(captured.value().recording_end_ns, 1100ULL);
    auto encoded = artifact::encode_manifest(captured.value(), limits());
    ASSERT_TRUE(encoded.ok());
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
    EXPECT_EQ(metadata.snapshot().native_inflight, 1ULL);
    EXPECT_EQ(
        metadata.capture_manifest(files(), committed_at()).status().code(),
        StatusCode::InvalidState);
    ASSERT_TRUE(claim.finish(RecordSettlementKind::Unknown, 0).ok());
    EXPECT_EQ(metadata.snapshot().native_inflight, 0ULL);
    auto captured = metadata.capture_manifest(files(), committed_at());
    ASSERT_TRUE(captured.ok());
    EXPECT_EQ(captured.value().coverage[0].unknown, 1ULL);
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
    EXPECT_EQ(snapshot.accepted, 2ULL);
    EXPECT_EQ(snapshot.unresolved_owners, 2ULL);
    EXPECT_EQ(snapshot.native_inflight, 1ULL);
    EXPECT_EQ(snapshot.known_failed, 0ULL);
    EXPECT_EQ(snapshot.unknown, 0ULL);
    EXPECT_EQ(
        metadata.capture_manifest(files(), committed_at()).status().code(),
        StatusCode::InvalidState);
}

TEST(SegmentMetadata, TransferredFailedDescriptorRetainsCountAndUnusedReadyFactsStayOutOfManifest) {
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
    EXPECT_EQ(created.value().snapshot().descriptors, 1U);
    EXPECT_EQ(created.value().snapshot().pending_descriptors, 0U);
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
    auto captured = ready.value().capture_manifest(files(), committed_at());
    ASSERT_TRUE(captured.ok());
    EXPECT_EQ(captured.value().descriptors.size(), 1U);
    EXPECT_EQ(captured.value().coverage.size(), 1U);
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
    EXPECT_EQ(snapshot.accepted, 200ULL);
    EXPECT_EQ(snapshot.written, 200ULL);
    EXPECT_EQ(snapshot.pending_admissions, 0U);
    EXPECT_EQ(snapshot.native_inflight, 0ULL);
    EXPECT_EQ(snapshot.unresolved_owners, 0ULL);
    EXPECT_TRUE(metadata.capture_manifest(files(), committed_at()).ok());
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
    EXPECT_EQ(metadata.snapshot().native_inflight, 1ULL);
    EXPECT_EQ(metadata.snapshot().written, 0ULL);
    EXPECT_EQ(
        metadata.capture_manifest(files(), committed_at()).status().code(),
        StatusCode::InvalidState);
    release.set_value();
    EXPECT_TRUE(actual.get());
    EXPECT_EQ(metadata.snapshot().native_inflight, 0ULL);
    EXPECT_EQ(metadata.snapshot().written, 1ULL);
    EXPECT_TRUE(metadata.capture_manifest(files(), committed_at()).ok());
}

}  // namespace
