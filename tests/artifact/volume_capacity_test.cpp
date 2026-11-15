#include "artifact/volume_capacity.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <future>
#include <limits>
#include <optional>
#include <vector>
#include <utility>

namespace {

namespace artifact = rodaq::artifact;
using rodaq::StatusCode;

artifact::VolumeCapacityOptions options() { return {10000, 10000, 100, 10, 8, 8, 16}; }

void sample(
    artifact::VolumeCapacity& capacity, std::uint64_t free_bytes = 1000,
    std::uint64_t timestamp = 100) {
    auto probe = capacity.begin_space_sample(timestamp);
    ASSERT_TRUE(probe.ok());
    ASSERT_TRUE(capacity.finish_space_sample(probe.value(), free_bytes, timestamp).ok());
}

TEST(VolumeCapacity, InitialReservationIsIncrementalAndPreIoRollbackRestoresEveryDimension) {
    auto created = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(created.ok());
    auto& volume = created.value();
    EXPECT_EQ(volume.reserve(50, 100).status().code(), StatusCode::Unavailable);
    sample(volume);
    {
        auto reserved = volume.reserve(50, 100);
        ASSERT_TRUE(reserved.ok());
        auto& segment = reserved.value();
        EXPECT_EQ(volume.snapshot().future_bytes, 50);
        EXPECT_EQ(volume.snapshot().commit_bytes, 50);
        EXPECT_EQ(volume.snapshot().segment_slots, 1);
        EXPECT_EQ(volume.snapshot().artifact_slots, 1);
        ASSERT_TRUE(segment.grow(100, 101).ok());
        EXPECT_EQ(volume.snapshot().future_bytes, 150);
    }
    EXPECT_EQ(volume.snapshot().future_bytes, 0);
    EXPECT_EQ(volume.snapshot().commit_bytes, 0);
    EXPECT_EQ(volume.snapshot().segment_slots, 0);
    EXPECT_EQ(volume.snapshot().commit_slots, 0);
    EXPECT_EQ(volume.snapshot().artifact_slots, 0);
    EXPECT_EQ(volume.snapshot().high_water_commit_bytes, 150);
}

TEST(
    VolumeCapacity, WrittenGrowthConvertsReservationWithoutDoubleChargeAndStaleSamplesCannotAdmit) {
    auto created = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(created.ok());
    auto& volume = created.value();
    sample(volume);
    auto acquired = volume.reserve(500, 100);
    ASSERT_TRUE(acquired.ok());
    auto& segment = acquired.value();
    ASSERT_TRUE(segment.record_usage(500, 0).ok());
    EXPECT_EQ(volume.snapshot().actual_bytes, 500);
    EXPECT_EQ(volume.snapshot().future_bytes, 0);
    EXPECT_EQ(volume.snapshot().commit_bytes, 500);
    EXPECT_EQ(volume.snapshot().consumed_since_sample, 500);
    EXPECT_EQ(volume.reserve(401, 101).status().code(), StatusCode::ResourceExhausted);
    EXPECT_TRUE(volume.reserve(400, 101).ok());
    // A staging->final rename changes no physical accounting.
    ASSERT_TRUE(segment.record_usage(500, 0).ok());
    EXPECT_EQ(volume.snapshot().consumed_since_sample, 500);
    sample(volume, 500, 102);
    EXPECT_EQ(volume.snapshot().consumed_since_sample, 0);
    EXPECT_TRUE(volume.reserve(400, 110).ok());
    EXPECT_EQ(volume.reserve(1, 113).status().code(), StatusCode::Unavailable);
    ASSERT_TRUE(segment.settle_aborted_after_cleanup().ok());
}

TEST(VolumeCapacity, SamplingWindowIncludesConcurrentWrittenDebtAndUsesConservativeStartFreshness) {
    auto created = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(created.ok());
    auto& volume = created.value();
    sample(volume);
    auto probe = volume.begin_space_sample(101);
    ASSERT_TRUE(probe.ok());
    auto acquired = volume.reserve(400, 101);
    ASSERT_TRUE(acquired.ok());
    ASSERT_TRUE(acquired.value().record_usage(400, 0).ok());
    ASSERT_TRUE(volume.finish_space_sample(probe.value(), 600, 105).ok());
    EXPECT_EQ(volume.snapshot().sampled_monotonic_ns, 101);
    EXPECT_EQ(volume.snapshot().consumed_since_sample, 400);
    EXPECT_EQ(volume.reserve(101, 106).status().code(), StatusCode::ResourceExhausted);
    EXPECT_TRUE(volume.reserve(100, 106).ok());
    sample(volume, 600, 107);
    EXPECT_TRUE(volume.reserve(500, 108).ok());
    EXPECT_EQ(volume.reserve(1, 118).status().code(), StatusCode::Unavailable);
    ASSERT_TRUE(acquired.value().settle_aborted_after_cleanup().ok());
    auto slow = volume.begin_space_sample(120);
    ASSERT_TRUE(slow.ok());
    EXPECT_EQ(
        volume.finish_space_sample(slow.value(), 1000, 131).status().code(),
        StatusCode::Unavailable);
}

TEST(VolumeCapacity, OutOfOrderOrOtherVolumeSamplesCannotReplaceCurrentFactsOrClearProtection) {
    auto first = artifact::VolumeCapacity::create(options());
    auto second = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    auto old = first.value().begin_space_sample(100);
    auto recent = first.value().begin_space_sample(101);
    ASSERT_TRUE(old.ok());
    ASSERT_TRUE(recent.ok());
    EXPECT_EQ(
        second.value().finish_space_sample(recent.value(), 1000, 101).status().code(),
        StatusCode::Conflict);
    ASSERT_TRUE(first.value().finish_space_sample(recent.value(), 1000, 101).ok());
    EXPECT_EQ(
        first.value().finish_space_sample(old.value(), 2000, 102).status().code(),
        StatusCode::Conflict);
    EXPECT_EQ(first.value().snapshot().sampled_free_bytes, 1000);
    first.value().protect(StatusCode::DataLoss);
    sample(first.value(), 10000, 105);
    EXPECT_EQ(first.value().reserve(1, 105).status().code(), StatusCode::DataLoss);
}

TEST(VolumeCapacity, RetainedActualAndTombstoneSlotOutliveCommitUntilExplicitDeleteAndGcProof) {
    auto created = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(created.ok());
    auto& volume = created.value();
    sample(volume);
    auto acquired = volume.reserve(100, 100);
    ASSERT_TRUE(acquired.ok());
    ASSERT_TRUE(acquired.value().record_usage(100, 0).ok());
    auto artifact = acquired.value().settle_retained();
    ASSERT_TRUE(artifact.ok());
    EXPECT_EQ(volume.snapshot().commit_slots, 0);
    EXPECT_EQ(volume.snapshot().commit_bytes, 0);
    EXPECT_EQ(volume.snapshot().actual_bytes, 100);
    EXPECT_EQ(volume.snapshot().artifact_slots, 1);
    EXPECT_EQ(artifact.value().release_after_gc().status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(artifact.value().release_after_delete().ok());
    EXPECT_EQ(volume.snapshot().actual_bytes, 0);
    EXPECT_EQ(volume.snapshot().artifact_slots, 1);
    ASSERT_TRUE(artifact.value().release_after_gc().ok());
    EXPECT_EQ(volume.snapshot().artifact_slots, 0);
    EXPECT_EQ(artifact.value().release_after_gc().status().code(), StatusCode::InvalidState);
}

TEST(
    VolumeCapacity,
    DroppedEffectfulOwnerAndEnvelopeViolationFailClosedInsteadOfReturningFakeCapacity) {
    auto created = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(created.ok());
    auto& volume = created.value();
    sample(volume);
    {
        auto acquired = volume.reserve(100, 100);
        ASSERT_TRUE(acquired.ok());
        acquired.value().mark_effects_possible();
    }
    EXPECT_EQ(volume.snapshot().segment_slots, 1);
    EXPECT_EQ(volume.snapshot().future_bytes, 100);
    EXPECT_EQ(volume.snapshot().orphaned_reservations, 1);
    EXPECT_FALSE(volume.snapshot().actual_usage_complete);
    EXPECT_EQ(volume.reserve(1, 101).status().code(), StatusCode::DataLoss);
    auto another = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(another.ok());
    sample(another.value());
    auto acquired = another.value().reserve(100, 100);
    ASSERT_TRUE(acquired.ok());
    EXPECT_EQ(acquired.value().record_usage(101, 0).status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(another.value().snapshot().actual_usage_complete);
    EXPECT_EQ(another.value().reserve(1, 100).status().code(), StatusCode::DataLoss);
    ASSERT_TRUE(acquired.value().settle_aborted_after_cleanup().ok());
}

TEST(
    VolumeCapacity,
    ByteAndCardinalityCapsRejectBeforeEffectsAndRecoveredOverQuotaRemainsDeletable) {
    auto policy = options();
    policy.max_managed_bytes = 100;
    policy.max_commit_bytes = 80;
    policy.max_segments = 1;
    policy.max_commit_slots = 1;
    auto created = artifact::VolumeCapacity::create(policy);
    ASSERT_TRUE(created.ok());
    sample(created.value());
    auto acquired = created.value().reserve(50, 100);
    ASSERT_TRUE(acquired.ok());
    EXPECT_EQ(acquired.value().grow(31, 100).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(created.value().reserve(1, 100).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(created.value().snapshot().future_bytes, 50);
    auto recovered = artifact::VolumeCapacity::create(policy, 200, 2);
    ASSERT_TRUE(recovered.ok());
    sample(recovered.value());
    EXPECT_EQ(recovered.value().reserve(1, 100).status().code(), StatusCode::ResourceExhausted);
    auto first = recovered.value().claim_recovered_artifact(100);
    auto second = recovered.value().claim_recovered_artifact(100);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(first.value().release_after_delete().ok());
    ASSERT_TRUE(second.value().release_after_delete().ok());
    ASSERT_TRUE(first.value().release_after_gc().ok());
    ASSERT_TRUE(second.value().release_after_gc().ok());
    EXPECT_EQ(recovered.value().snapshot().actual_bytes, 0);
    EXPECT_EQ(recovered.value().snapshot().artifact_slots, 0);
    EXPECT_TRUE(recovered.value().reserve(50, 100).ok());
}

TEST(VolumeCapacity, ConcurrentSegmentsShareCommitAndSpaceBudgetWithoutLeakingPreIoCharges) {
    auto created = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(created.ok());
    auto& volume = created.value();
    sample(volume);
    std::vector<std::future<bool>> workers;
    for (unsigned thread = 0; thread < 8; ++thread) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (unsigned iteration = 0; iteration < 128; ++iteration) {
                auto acquired = volume.reserve(10, 100);
                if (!acquired.ok() || !acquired.value().grow(10, 100).ok()) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& worker : workers) {
        EXPECT_TRUE(worker.get());
    }
    EXPECT_EQ(volume.snapshot().future_bytes, 0);
    EXPECT_EQ(volume.snapshot().commit_bytes, 0);
    EXPECT_EQ(volume.snapshot().segment_slots, 0);
    EXPECT_EQ(volume.snapshot().artifact_slots, 0);
}

TEST(VolumeCapacity, EmptyArtifactStillRequiresDeleteProofBeforeTombstoneGc) {
    auto created = artifact::VolumeCapacity::create(options(), 0, 1);
    ASSERT_TRUE(created.ok());
    auto recovered = created.value().claim_recovered_artifact(0);
    ASSERT_TRUE(recovered.ok());
    auto owner = std::move(recovered.value());
    EXPECT_EQ(owner.release_after_gc().status().code(), StatusCode::InvalidState);
    EXPECT_EQ(created.value().snapshot().artifact_slots, 1);
    ASSERT_TRUE(owner.release_after_delete().ok());
    auto transferred = std::move(owner);
    ASSERT_TRUE(transferred.release_after_gc().ok());
    EXPECT_EQ(created.value().snapshot().artifact_slots, 0);
}

TEST(VolumeCapacity, SampleFailureInvalidatesOldAndOutstandingEvidenceButAllowsFreshRecovery) {
    auto created = artifact::VolumeCapacity::create(options());
    ASSERT_TRUE(created.ok());
    auto& volume = created.value();
    sample(volume);
    auto old = volume.begin_space_sample(101);
    ASSERT_TRUE(old.ok());
    volume.invalidate_space_sample();
    EXPECT_EQ(volume.reserve(1, 102).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(
        volume.finish_space_sample(old.value(), 1000, 102).status().code(), StatusCode::Conflict);
    sample(volume, 1000, 103);
    EXPECT_TRUE(volume.reserve(1, 103).ok());
    EXPECT_EQ(volume.snapshot().protection, StatusCode::Ok);
}

}  // namespace
