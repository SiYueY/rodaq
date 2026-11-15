#include "pipeline/segment_capacity.hpp"
#include "pipeline/admission_gate.hpp"

#include <future>
#include <chrono>

#include <gtest/gtest.h>

#include <optional>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>

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

class SegmentCapacityTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 48> pattern{};
        const std::string prefix = "/tmp/rodaq-segment-ledger-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        auto opened = artifact::LedgerStore::open(root_, artifact::LedgerOpenMode::Initialize, 8);
        ASSERT_TRUE(opened.ok());
        ledger_ = std::move(opened.value());
    }

    void TearDown() override {
        ledger_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    rodaq::Result<pipeline::SegmentCapacity> create_segment(
        std::uint64_t id, rodaq::storage::PrepareConfig envelope,
        rodaq::storage::CapacityModel capacity, std::uint64_t manifest,
        artifact::VolumeCapacity& volume, std::uint64_t now) {
        auto registration = ledger_->reserve_registration(id);
        if (!registration.ok()) {
            return registration.status();
        }
        return pipeline::SegmentCapacity::create(
            id, envelope, capacity, manifest, volume, std::move(registration.value()), now);
    }

    std::filesystem::path root_;
    std::unique_ptr<artifact::LedgerStore> ledger_;
};

TEST_F(
    SegmentCapacityTest,
    PrepareReservesOnlyFixedSealAndManifestAndCandidatesRollbackEveryDimension) {
    auto volume = make_volume();
    auto created = create_segment(1, limits(), model(), 200, volume, 100);
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

TEST_F(
    SegmentCapacityTest, RecordAndDescriptorCapsRejectBeforeTransferAndRetainAcceptedSettlement) {
    auto volume = make_volume();
    auto created = create_segment(2, limits(), model(), 200, volume, 100);
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
    auto registration = segment.take_registration_for_commit();
    ASSERT_TRUE(registration.ok());
    auto retained = segment.settle_retained();
    ASSERT_TRUE(retained.ok());
    EXPECT_EQ(volume.snapshot().commit_bytes, 0);
    EXPECT_EQ(volume.snapshot().actual_bytes, 500);
    ASSERT_TRUE(retained.value().release_after_delete().ok());
    ASSERT_TRUE(retained.value().release_after_gc().ok());
}

TEST_F(SegmentCapacityTest, CutRejectsOldCandidatesAndSealWaitsForTheirRollback) {
    auto volume = make_volume();
    auto created = create_segment(3, limits(), model(), 200, volume, 100);
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
    auto registration = segment.take_registration_for_commit();
    ASSERT_TRUE(registration.ok());
    EXPECT_TRUE(segment.settle_retained().ok());
}

TEST_F(SegmentCapacityTest, NativeUsageCannotConsumeUnacceptedCandidateOrFinalizationReserve) {
    auto volume = make_volume();
    auto created = create_segment(4, limits(), model(), 200, volume, 100);
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

TEST_F(
    SegmentCapacityTest,
    UsageConvertsReservedExposureWithoutDoubleChargeAndSealIncludesCoreManifest) {
    auto volume = make_volume();
    auto created = create_segment(5, limits(), model(), 200, volume, 100);
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

TEST_F(SegmentCapacityTest, PhysicalHardLimitAndModelOverflowCannotAdmit) {
    auto volume = make_volume();
    auto envelope = limits();
    envelope.max_segment_physical_bytes = 100;
    auto created = create_segment(6, envelope, model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    EXPECT_EQ(
        created.value().reserve_record(6, 100).status().code(), StatusCode::ResourceExhausted);
    auto invalid = model();
    invalid.payload_expansion_denominator = 0;
    EXPECT_EQ(
        create_segment(7, limits(), invalid, 0, volume, 100).status().code(),
        StatusCode::InvalidArgument);
}

TEST_F(SegmentCapacityTest, GateCutWaitsForCoreCommitThenRejectsCapturedOldSegmentCandidate) {
    auto volume = make_volume();
    auto created = create_segment(8, limits(), model(), 200, volume, 100);
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

TEST_F(SegmentCapacityTest, RegistrationSlotMustMatchSegmentAndRollsBackWhenVolumeCannotReserve) {
    auto volume = make_volume();
    auto wrong_slot = ledger_->reserve_registration(99);
    ASSERT_TRUE(wrong_slot.ok());
    EXPECT_EQ(
        pipeline::SegmentCapacity::create(
            9, limits(), model(), 200, volume, std::move(wrong_slot.value()), 100)
            .status()
            .code(),
        StatusCode::InvalidArgument);
    EXPECT_TRUE(ledger_->reserve_registration(99).ok());
    EXPECT_EQ(volume.snapshot().future_bytes, 0);
    auto small = artifact::VolumeCapacity::create({100, 100, 0, 100, 8, 8, 16});
    ASSERT_TRUE(small.ok());
    auto probe = small.value().begin_space_sample(100);
    ASSERT_TRUE(probe.ok());
    ASSERT_TRUE(small.value().finish_space_sample(probe.value(), 10000, 100).ok());
    EXPECT_EQ(
        create_segment(10, limits(), model(), 200, small.value(), 100).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_TRUE(ledger_->reserve_registration(10).ok());
}

TEST_F(SegmentCapacityTest, RegistrationTransfersOnceOnlyAfterSealAndBeforeRetainedSettlement) {
    auto volume = make_volume();
    auto created = create_segment(11, limits(), model(), 200, volume, 100);
    ASSERT_TRUE(created.ok());
    auto& segment = created.value();
    EXPECT_EQ(ledger_->reserve_registration(11).status().code(), StatusCode::Conflict);
    EXPECT_EQ(segment.take_registration_for_commit().status().code(), StatusCode::InvalidState);
    segment.close_admission();
    ASSERT_TRUE(segment.finish_seal(50, 100).ok());
    EXPECT_EQ(segment.settle_retained().status().code(), StatusCode::InvalidState);
    auto registration = segment.take_registration_for_commit();
    ASSERT_TRUE(registration.ok());
    EXPECT_EQ(registration.value().segment_id(), 11);
    EXPECT_EQ(segment.take_registration_for_commit().status().code(), StatusCode::InvalidState);
    EXPECT_EQ(ledger_->reserve_registration(11).status().code(), StatusCode::Conflict);
    EXPECT_TRUE(segment.settle_retained().ok());
}

}  // namespace
