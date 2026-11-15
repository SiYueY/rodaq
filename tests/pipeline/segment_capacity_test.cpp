#include "pipeline/segment_capacity.hpp"
#include "pipeline/admission_gate.hpp"

#include <future>
#include <chrono>

#include <gtest/gtest.h>

#include <optional>

namespace {

using rodaq::StatusCode;
namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;

artifact::VolumeCapacity make_volume() {
    auto volume = artifact::VolumeCapacity::create({100000, 100000, 100, 100, 8, 8, 16});
    auto probe = volume.value().begin_space_sample(100);
    EXPECT_TRUE(volume.value().finish_space_sample(probe.value(), 100000, 100).ok());
    return std::move(volume.value());
}

rodaq::storage::PrepareConfig limits() {
    rodaq::storage::PrepareConfig result{};
    result.max_record_bytes = 100;
    result.max_segment_records = 2;
    result.max_segment_input_bytes = 150;
    result.max_segment_physical_bytes = 1000;
    result.max_descriptors_per_segment = 2;
    result.max_segment_file_count = 4;
    return result;
}

rodaq::storage::CapacityModel model() {
    rodaq::storage::CapacityModel result{};
    result.payload_expansion_numerator = 2;
    result.payload_expansion_denominator = 1;
    result.per_record_overhead_bytes = 10;
    result.descriptor_expansion_numerator = 1;
    result.descriptor_expansion_denominator = 1;
    result.per_descriptor_overhead_bytes = 20;
    result.fixed_segment_overhead_bytes = 50;
    result.seal_reserve_bytes = 30;
    return result;
}

TEST(SegmentCapacity, PrepareReservesOnlyFixedSealAndManifestAndCandidatesRollbackEveryDimension) {
    auto volume = make_volume();
    auto created = pipeline::SegmentCapacity::create(1, limits(), model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    auto& segment = created.value();
    EXPECT_EQ(volume.snapshot().future_bytes, 280);
    {
        auto record = segment.reserve_record(75, 101);
        auto descriptor = segment.reserve_descriptor(20, 101);
        ASSERT_TRUE(record.ok());
        ASSERT_TRUE(descriptor.ok());
        EXPECT_EQ(volume.snapshot().future_bytes, 480);
        EXPECT_EQ(segment.snapshot().records, 1);
        EXPECT_EQ(segment.snapshot().descriptors, 1);
        EXPECT_EQ(segment.snapshot().candidates, 2);
    }
    EXPECT_EQ(segment.snapshot().records, 0);
    EXPECT_EQ(segment.snapshot().input_bytes, 0);
    EXPECT_EQ(segment.snapshot().descriptors, 0);
    EXPECT_EQ(volume.snapshot().future_bytes, 280);
}

TEST(SegmentCapacity, RecordAndDescriptorCapsRejectBeforeTransferAndRetainAcceptedSettlement) {
    auto volume = make_volume();
    auto created = pipeline::SegmentCapacity::create(2, limits(), model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    auto& segment = created.value();
    EXPECT_EQ(segment.reserve_record(101, 100).status().code(), StatusCode::ResourceExhausted);
    auto first = segment.reserve_record(100, 100);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value().transfer().ok());
    EXPECT_EQ(segment.reserve_record(51, 100).status().code(), StatusCode::ResourceExhausted);
    auto second = segment.reserve_record(50, 100);
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(second.value().transfer().ok());
    EXPECT_EQ(segment.reserve_record(0, 100).status().code(), StatusCode::ResourceExhausted);
    auto descriptor = segment.reserve_descriptor(1, 100);
    ASSERT_TRUE(descriptor.ok());
    ASSERT_TRUE(descriptor.value().transfer().ok());
    auto second_descriptor = segment.reserve_descriptor(1, 100);
    ASSERT_TRUE(second_descriptor.ok());
    ASSERT_TRUE(second_descriptor.value().transfer().ok());
    EXPECT_EQ(segment.reserve_descriptor(0, 100).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(segment.snapshot().candidates, 0);
    segment.close_admission();
    ASSERT_TRUE(segment.finish_seal(400, 100).ok());
    EXPECT_EQ(volume.snapshot().actual_bytes, 500);
    EXPECT_EQ(volume.snapshot().future_bytes, 0);
    auto retained = segment.settle_retained();
    ASSERT_TRUE(retained.ok());
    EXPECT_EQ(volume.snapshot().commit_bytes, 0);
    EXPECT_EQ(volume.snapshot().actual_bytes, 500);
    ASSERT_TRUE(retained.value().release_after_delete().ok());
    ASSERT_TRUE(retained.value().release_after_gc().ok());
}

TEST(SegmentCapacity, CutRejectsOldCandidatesAndSealWaitsForTheirRollback) {
    auto volume = make_volume();
    auto created = pipeline::SegmentCapacity::create(3, limits(), model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    auto& segment = created.value();
    {
        auto candidate = segment.reserve_record(50, 100);
        ASSERT_TRUE(candidate.ok());
        segment.close_admission();
        EXPECT_EQ(candidate.value().transfer().status().code(), StatusCode::Conflict);
        EXPECT_EQ(segment.finish_seal(50, 0).status().code(), StatusCode::InvalidState);
    }
    ASSERT_TRUE(segment.finish_seal(50, 0).ok());
    EXPECT_TRUE(segment.settle_retained().ok());
}

TEST(SegmentCapacity, NativeUsageCannotConsumeUnacceptedCandidateOrFinalizationReserve) {
    auto volume = make_volume();
    auto created = pipeline::SegmentCapacity::create(4, limits(), model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    auto& segment = created.value();
    auto candidate = segment.reserve_record(100, 100);
    ASSERT_TRUE(candidate.ok());
    rodaq::storage::SegmentUsage usage{};
    usage.physical_bytes = 51;
    EXPECT_EQ(segment.observe_usage(usage).status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(segment.snapshot().writable);
    EXPECT_EQ(volume.snapshot().protection, StatusCode::DataLoss);
    EXPECT_FALSE(volume.snapshot().actual_usage_complete);
    EXPECT_EQ(candidate.value().transfer().status().code(), StatusCode::Conflict);
}

TEST(SegmentCapacity, UsageConvertsReservedExposureWithoutDoubleChargeAndSealIncludesCoreManifest) {
    auto volume = make_volume();
    auto created = pipeline::SegmentCapacity::create(5, limits(), model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    auto& segment = created.value();
    auto candidate = segment.reserve_record(100, 100);
    ASSERT_TRUE(candidate.ok());
    ASSERT_TRUE(candidate.value().transfer().ok());
    segment.mark_effects_possible();
    rodaq::storage::SegmentUsage usage{};
    usage.physical_bytes = 150;
    usage.pending_physical_upper_bound = 100;
    ASSERT_TRUE(segment.observe_usage(usage).ok());
    EXPECT_EQ(volume.snapshot().actual_bytes, 150);
    EXPECT_EQ(volume.snapshot().future_bytes, 340);
    EXPECT_EQ(volume.snapshot().commit_bytes, 490);
    usage.physical_bytes = 100;
    ASSERT_TRUE(segment.observe_usage(usage).ok());
    EXPECT_EQ(volume.snapshot().actual_bytes, 100);
    EXPECT_EQ(volume.snapshot().future_bytes, 390);
    EXPECT_EQ(volume.snapshot().consumed_since_sample, 150);
    segment.close_admission();
    ASSERT_TRUE(segment.finish_seal(275, 180).ok());
    EXPECT_EQ(volume.snapshot().actual_bytes, 455);
    EXPECT_EQ(volume.snapshot().future_bytes, 0);
    ASSERT_TRUE(segment.settle_aborted_after_cleanup().ok());
    EXPECT_EQ(volume.snapshot().actual_bytes, 0);
}

TEST(SegmentCapacity, PhysicalHardLimitAndModelOverflowCannotAdmit) {
    auto volume = make_volume();
    auto envelope = limits();
    envelope.max_segment_physical_bytes = 100;
    auto created = pipeline::SegmentCapacity::create(6, envelope, model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(
        created.value().reserve_record(6, 100).status().code(), StatusCode::ResourceExhausted);
    auto invalid = model();
    invalid.payload_expansion_denominator = 0;
    EXPECT_EQ(
        pipeline::SegmentCapacity::create(7, limits(), invalid, 0, volume, 100).status().code(),
        StatusCode::InvalidArgument);
}

TEST(SegmentCapacity, GateCutWaitsForCoreCommitThenRejectsCapturedOldSegmentCandidate) {
    auto volume = make_volume();
    auto created = pipeline::SegmentCapacity::create(8, limits(), model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    auto& segment = created.value();
    pipeline::InstanceId id;
    id.value[15] = 1;
    pipeline::AdmissionGate gate(id, 1);
    ASSERT_EQ(gate.open_admission(id, 1), pipeline::GateOpenResult::Opened);
    auto candidate = segment.reserve_record(10, 100);
    ASSERT_TRUE(candidate.ok());
    std::optional<rodaq::Result<pipeline::AdmissionCommit>> committing;
    committing.emplace(gate.enter_commit(id, 1));
    ASSERT_TRUE(committing->ok());
    std::promise<void> attempted;
    auto cut = std::async(std::launch::async, [&] {
        attempted.set_value();
        auto barrier = gate.enter_segment_cut(id, 1);
        if (!barrier.ok()) {
            return false;
        }
        segment.close_admission();
        return true;
    });
    attempted.get_future().wait();
    EXPECT_EQ(cut.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
    committing.reset();
    EXPECT_TRUE(cut.get());
    EXPECT_EQ(candidate.value().transfer().status().code(), StatusCode::Conflict);
    EXPECT_TRUE(gate.snapshot().open);
    EXPECT_EQ(gate.snapshot().accepted_records, 0);
}

}  // namespace
