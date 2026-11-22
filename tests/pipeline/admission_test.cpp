#include "pipeline/admission.hpp"
#include "artifact/segment_committer.hpp"
#include "persistence/atomic_file.hpp"
#include "pipeline/descriptor_registry.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <optional>
#include <new>
#include <thread>
#include <vector>

#include <rodaq/storage/validation.hpp>

#ifdef RODAQ_TEST_ADMISSION_ALLOCATION
namespace admission_allocation {
extern thread_local bool reject;
}
#endif

namespace {

namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;

class AdmissionTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 48> pattern{};
        const std::string prefix = "/tmp/rodaq-admission-ledger-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* prepared = mkdtemp(pattern.data());
        ASSERT_NE(prepared, nullptr);
        ledger_root_ = prepared;
        std::filesystem::create_directory(ledger_root_ / "ledger");
        std::filesystem::create_directory(ledger_root_ / "volume");
        auto managed = artifact::ManagedVolume::open(
            ledger_root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(managed.ok());
        managed_volume_ = std::move(managed.value());
        auto ledger = artifact::LedgerStore::open(
            ledger_root_ / "ledger", artifact::LedgerOpenMode::Initialize, 4);
        ASSERT_TRUE(ledger.ok());
        ledger_ = std::move(ledger.value());
        auto volume = artifact::VolumeCapacity::create({1000000, 1000000, 100, 1000, 8, 8, 16});
        ASSERT_TRUE(volume.ok());
        volume_.emplace(std::move(volume.value()));
        auto probe = volume_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(volume_->finish_space_sample(probe.value(), 1000000, 100).ok());
        instance_.value[15] = 1;
        auto identity = rodaq::test::manifest_identity();
        identity.volume_id = managed_volume_->identity();
        identity.pipeline_id = 1;
        identity.instance_id = pipeline::to_string(instance_);
        identity.control_generation = 1;
        identity.reset_generation_at_commit = 0;
        identity.playback_anchor = {1, 0, 1000};
        rodaq::storage::PrepareConfig limits{};
        limits.header.struct_size = sizeof(limits);
        limits.max_batch_bytes = 64;
        limits.max_batch_records = 8;
        limits.max_segment_file_count = 2;
        limits.max_relative_path_bytes = 64;
        limits.system_boot_id = 1;
        limits.playback_anchor_realtime = 1000;
        limits.max_record_bytes = 8;
        limits.max_segment_records = 1000;
        limits.max_segment_input_bytes = 8000;
        limits.max_segment_physical_bytes = 100000;
        limits.max_descriptors_per_segment = 8;
        storage_limits_ = limits;
        rodaq::storage::CapacityModel model{};
        model.header.struct_size = sizeof(model);
        model.payload_expansion_numerator = 1;
        model.payload_expansion_denominator = 1;
        model.per_record_overhead_bytes = 10;
        model.descriptor_expansion_numerator = 1;
        model.descriptor_expansion_denominator = 1;
        model.fixed_segment_overhead_bytes = 50;
        model.seal_reserve_bytes = 50;
        auto registration = ledger_->reserve_registration(7);
        ASSERT_TRUE(registration.ok());
        artifact::ManifestLimits manifest_limits;
        manifest_limits.max_bytes = 8192;
        manifest_limits.max_files = 2;
        manifest_limits.max_descriptors = 8;
        manifest_limits.max_streams = 1;
        manifest_limits.max_epochs = 2;
        auto segment = pipeline::RecordingSegment::prepare(
            std::move(identity), {1}, limits, model, manifest_limits, *volume_,
            std::move(registration.value()), 100);
        ASSERT_TRUE(segment.ok());
        stream_.segment = std::move(segment.value());
        auto registry = pipeline::DescriptorRegistry::create({1});
        ASSERT_TRUE(registry.ok());
        registry_.emplace(std::move(registry.value()));
        stream_.active_descriptor = descriptor(1);
        ASSERT_TRUE(stream_.active_descriptor);
        auto facts = artifact::capture_manifest_descriptor(stream_.active_descriptor->view(), 0);
        ASSERT_TRUE(facts.ok());
        auto descriptor_metadata =
            stream_.segment->metadata.reserve_descriptor(std::move(facts.value()));
        auto descriptor_physical = stream_.segment->capacity.reserve_descriptor(9, 100);
        ASSERT_TRUE(descriptor_metadata.ok());
        ASSERT_TRUE(descriptor_physical.ok());
        ASSERT_TRUE(descriptor_physical.value().transfer().ok());
        descriptor_metadata.value().transfer();
        ASSERT_TRUE(descriptor_metadata.value().mark_prepared().ok());
        ASSERT_TRUE(stream_.sequences.begin_refill(1).ok());
        ASSERT_TRUE(stream_.sequences.finish_refill(1, {1, 1001}).ok());
        budget_.emplace(std::move(pipeline::RecordBudget::create(16, 128).value()));
        pool_.emplace(std::move(pipeline::PayloadPool::create({8192, 256, 8, 16}).value()));
        queue_.emplace(std::move(pipeline::RecordQueue::create(8).value()));
        instance_.value[15] = 1;
        gate_.emplace(instance_, 1);
        ASSERT_EQ(gate_->open_admission(instance_, 1), pipeline::GateOpenResult::Opened);
        const auto task = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        domain_.emplace(pipeline::AdmissionDomain{
            task, 1, instance_, 1, *gate_, *budget_, *pool_, *queue_, 0, 100});
        received_.boot_id = 1;
        received_.epoch_id = 1;
        received_.sample = {100, 1000, 1};
    }

    void TearDown() override {
        stream_.segment.reset();
        ledger_.reset();
        managed_volume_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(ledger_root_, ignored);
    }

    pipeline::DescriptorRef descriptor(std::uint64_t id) {
        rodaq::acquisition::DescriptorProposal proposal{};
        proposal.header.struct_size = sizeof(proposal);
        proposal.stream_id = 1;
        proposal.data_profile = RODAQ_DATA_PROFILE_GENERIC;
        proposal.channel_name = {"/data", 5};
        proposal.message_encoding = {"json", 4};
        auto result = registry_->capture(proposal, id);
        return result.ok() ? std::move(result.value()) : nullptr;
    }

    rodaq::acquisition::RecordAttempt attempt() const {
        rodaq::acquisition::RecordAttempt value{};
        value.header.struct_size = sizeof(value);
        value.stream_id = 1;
        value.descriptor_token = stream_.active_descriptor->token;
        value.payload = {{sizeof(rodaq_payload_t)}, bytes_.data(), bytes_.size()};
        return value;
    }

    void expect_rolled_back() {
        EXPECT_EQ(budget_->snapshot().records, 0);
        EXPECT_EQ(pool_->snapshot().live_buffers, 0);
        EXPECT_EQ(queue_->snapshot().reserved_slots, 0);
        EXPECT_EQ(queue_->snapshot().queued_records, 0);
        EXPECT_EQ(stream_.segment->capacity.snapshot().records, 0);
        EXPECT_EQ(stream_.segment->capacity.snapshot().candidates, 0);
        EXPECT_EQ(stream_.sequences.snapshot().active_remaining, 1000);
        EXPECT_EQ(volume_->snapshot().future_bytes, 8301);
        EXPECT_EQ(stream_.segment->metadata.snapshot().pending_admissions, 0);
        EXPECT_EQ(stream_.segment->metadata.snapshot().accepted, 0);
        EXPECT_EQ(stream_.segment->metadata.snapshot().epochs, 0);
        EXPECT_EQ(gate_->snapshot().accepted_records, 0);
    }

    bool wait_for_candidate() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (budget_->snapshot().records == 0 && std::chrono::steady_clock::now() < until) {
            std::this_thread::yield();
        }
        return budget_->snapshot().records != 0;
    }

    std::optional<artifact::VolumeCapacity> volume_;
    std::optional<pipeline::DescriptorRegistry> registry_;
    pipeline::StreamAdmission stream_{1, 8};
    std::optional<pipeline::RecordBudget> budget_;
    std::optional<pipeline::PayloadPool> pool_;
    std::optional<pipeline::RecordQueue> queue_;
    pipeline::InstanceId instance_;
    std::optional<pipeline::AdmissionGate> gate_;
    std::optional<pipeline::AdmissionDomain> domain_;
    rodaq::time::ClockSnapshot received_;
    std::array<std::uint8_t, 4> bytes_{1, 2, 3, 4};
    std::filesystem::path ledger_root_;
    std::unique_ptr<artifact::LedgerStore> ledger_;
    std::shared_ptr<artifact::ManagedVolume> managed_volume_;
    rodaq::storage::PrepareConfig storage_limits_{};
};

TEST_F(AdmissionTest, AcceptedRecordOwnsCopyAndRetainsSettlementAfterQueuePop) {
    auto input = attempt();
    auto result = pipeline::submit_record(*domain_, stream_, input, received_);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().disposition, RODAQ_SUBMIT_ACCEPTED);
    bytes_.fill(9);
    auto record = queue_->pop();
    ASSERT_TRUE(record);
    EXPECT_EQ(record->acceptance_sequence, 1);
    EXPECT_EQ(record->segment_id, 7);
    EXPECT_EQ(record->received.sample.realtime_ns, 1000);
    const auto view = record->view();
    EXPECT_TRUE(rodaq::storage::abi::valid_record(view));
    EXPECT_EQ(view.payload.data[0], 1);
    EXPECT_EQ(budget_->snapshot().records, 1);
    EXPECT_EQ(volume_->snapshot().future_bytes, 8315);
    EXPECT_EQ(gate_->snapshot().accepted_records, 1);
    ASSERT_TRUE(record->settlement.finish(pipeline::RecordSettlementKind::KnownFailed, 0).ok());
    record.reset();
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    EXPECT_EQ(stream_.segment->capacity.snapshot().records, 1);
}

TEST_F(AdmissionTest, InvalidStaleOversizeAndClosedGateNeverConsumeSequenceOrCapacity) {
    auto input = attempt();
    input.timestamp_count = 5;
    EXPECT_EQ(
        pipeline::submit_record(*domain_, stream_, input, received_).value().disposition,
        RODAQ_SUBMIT_INVALID_RECORD);
    input = attempt();
    ++input.descriptor_token.generation;
    EXPECT_EQ(
        pipeline::submit_record(*domain_, stream_, input, received_).value().disposition,
        RODAQ_SUBMIT_STALE_DESCRIPTOR);
    input = attempt();
    input.payload.logical_size = 9;
    EXPECT_EQ(
        pipeline::submit_record(*domain_, stream_, input, received_).value().disposition,
        RODAQ_SUBMIT_RECORD_TOO_LARGE);
    ASSERT_TRUE(gate_->establish_control_fence(instance_).ok());
    input = attempt();
    EXPECT_EQ(
        pipeline::submit_record(*domain_, stream_, input, received_).value().disposition,
        RODAQ_SUBMIT_GATE_CLOSED);
    expect_rolled_back();
}

TEST_F(AdmissionTest, QueueCapacityAndExhaustedSequenceRejectWithFullRollback) {
    std::vector<pipeline::QueueReservation> held;
    for (unsigned index = 0; index < 8; ++index) {
        auto slot = queue_->reserve();
        ASSERT_TRUE(slot.ok());
        held.push_back(std::move(slot.value()));
    }
    EXPECT_EQ(
        pipeline::submit_record(*domain_, stream_, attempt(), received_).value().disposition,
        RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    held.clear();
    expect_rolled_back();
    for (unsigned index = 0; index < 1000; ++index) {
        ASSERT_TRUE(stream_.sequences.allocate().ok());
    }
    EXPECT_EQ(
        pipeline::submit_record(*domain_, stream_, attempt(), received_).value().disposition,
        RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    EXPECT_EQ(stream_.segment->capacity.snapshot().records, 0);
    EXPECT_EQ(volume_->snapshot().future_bytes, 8301);
}

TEST_F(AdmissionTest, FenceDuringCandidatePreparationRejectsBeforeSequenceAndPublication) {
    auto input = attempt();
    std::optional<rodaq::Result<pipeline::AdmissionCommit>> held;
    held.emplace(gate_->enter_commit(instance_, 1));
    ASSERT_TRUE(held->ok());
    auto submission = std::async(std::launch::async, [&] {
        return pipeline::submit_record(*domain_, stream_, input, received_);
    });
    const bool started = wait_for_candidate();
    EXPECT_TRUE(started);
    EXPECT_TRUE(gate_->establish_control_fence(instance_).ok());
    held.reset();
    EXPECT_EQ(submission.get().value().disposition, RODAQ_SUBMIT_GATE_CLOSED);
    expect_rolled_back();
}

TEST_F(AdmissionTest, DescriptorCutBetweenCaptureAndCommitRejectsOldTokenWithoutBindingNewFacts) {
    auto input = attempt();
    auto replacement = descriptor(2);
    ASSERT_TRUE(replacement);
    std::optional<rodaq::Result<pipeline::AdmissionCommit>> held;
    held.emplace(gate_->enter_commit(instance_, 1));
    ASSERT_TRUE(held->ok());
    auto submission = std::async(std::launch::async, [&] {
        return pipeline::submit_record(*domain_, stream_, input, received_);
    });
    EXPECT_TRUE(wait_for_candidate());
    pipeline::DescriptorRef previous;
    {
        std::lock_guard<std::mutex> lock(stream_.mutex);
        previous = std::move(stream_.active_descriptor);
        stream_.active_descriptor = replacement;
    }
    held.reset();
    EXPECT_EQ(submission.get().value().disposition, RODAQ_SUBMIT_STALE_DESCRIPTOR);
    expect_rolled_back();
}

TEST_F(AdmissionTest, ConcurrentProducersPublishContinuousSameStreamSequencesAndBoundAllResources) {
    const auto input = attempt();
    std::atomic<bool> failed{false};
    auto consumer = std::async(std::launch::async, [&] {
        std::uint64_t expected = 1;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (expected <= 400 && std::chrono::steady_clock::now() < until) {
            auto record = queue_->pop();
            if (!record) {
                std::this_thread::yield();
                continue;
            }
            if (record->acceptance_sequence != expected++) {
                failed = true;
            }
            if (!record->settlement.finish(pipeline::RecordSettlementKind::KnownFailed, 0).ok()) {
                failed = true;
            }
        }
        if (expected != 401) {
            failed = true;
        }
    });
    std::vector<std::future<void>> producers;
    for (unsigned producer = 0; producer < 4; ++producer) {
        producers.push_back(std::async(std::launch::async, [&] {
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (unsigned index = 0; index < 100;) {
                auto result = pipeline::submit_record(*domain_, stream_, input, received_);
                if (!result.ok() || std::chrono::steady_clock::now() > until) {
                    failed = true;
                    return;
                }
                if (result.value().disposition == RODAQ_SUBMIT_ACCEPTED) {
                    ++index;
                } else if (result.value().disposition != RODAQ_SUBMIT_RESOURCE_EXHAUSTED) {
                    failed = true;
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        }));
    }
    for (auto& producer : producers) {
        producer.get();
    }
    consumer.get();
    EXPECT_FALSE(failed);
    EXPECT_EQ(gate_->snapshot().accepted_records, 400);
    EXPECT_EQ(stream_.segment->metadata.snapshot().accepted, 400);
    EXPECT_EQ(stream_.segment->metadata.snapshot().known_failed, 400);
    EXPECT_EQ(stream_.segment->metadata.snapshot().unresolved_owners, 0);
    EXPECT_EQ(stream_.sequences.snapshot().active_remaining, 600);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    EXPECT_EQ(queue_->snapshot().reserved_slots, 0);
    EXPECT_EQ(stream_.segment->capacity.snapshot().candidates, 0);
    EXPECT_EQ(stream_.segment->capacity.snapshot().records, 400);
    EXPECT_EQ(volume_->snapshot().future_bytes, 13901);
}

#ifdef RODAQ_TEST_ADMISSION_ALLOCATION
TEST_F(AdmissionTest, PreparedCopyAndCommitAndFenceRollbackWorkWithAllocationDisabled) {
    const auto input = attempt();
    rodaq::Result<rodaq::acquisition::SubmitResult> result(
        rodaq::Status(rodaq::StatusCode::Internal, {}));
    bool threw = false;
    admission_allocation::reject = true;
    try {
        result = pipeline::submit_record(*domain_, stream_, input, received_);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    admission_allocation::reject = false;
    EXPECT_FALSE(threw);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().disposition, RODAQ_SUBMIT_ACCEPTED);
    auto record = queue_->pop();
    ASSERT_TRUE(record);
    ASSERT_TRUE(record->settlement.finish(pipeline::RecordSettlementKind::KnownFailed, 0).ok());
    record.reset();
    ASSERT_TRUE(gate_->establish_control_fence(instance_).ok());
    admission_allocation::reject = true;
    try {
        result = pipeline::submit_record(*domain_, stream_, input, received_);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    admission_allocation::reject = false;
    EXPECT_FALSE(threw);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().disposition, RODAQ_SUBMIT_GATE_CLOSED);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    EXPECT_EQ(queue_->snapshot().reserved_slots, 0);
    EXPECT_EQ(stream_.segment->capacity.snapshot().records, 1);
    EXPECT_EQ(volume_->snapshot().future_bytes, 8315);
    EXPECT_EQ(stream_.sequences.snapshot().active_remaining, 999);
}
#endif

TEST_F(AdmissionTest, EpochMetadataPressureRejectsBeforeSequenceAndKeepsTheExactAcceptedEvidence) {
    auto first = pipeline::submit_record(*domain_, stream_, attempt(), received_);
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().disposition, RODAQ_SUBMIT_ACCEPTED);
    auto first_record = queue_->pop();
    ASSERT_TRUE(first_record);
    ASSERT_TRUE(
        first_record->settlement.finish(pipeline::RecordSettlementKind::KnownFailed, 0).ok());
    first_record.reset();
    auto changed = received_;
    changed.epoch_id = 2;
    changed.sample = {200, 2100, 2};
    changed.origin = rodaq::time::ClockEpochOrigin::RealtimeJump;
    changed.jump = rodaq::time::ClockJump{1, 1000, 150};
    auto second = pipeline::submit_record(*domain_, stream_, attempt(), changed);
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(second.value().disposition, RODAQ_SUBMIT_ACCEPTED);
    auto second_record = queue_->pop();
    ASSERT_TRUE(second_record);
    ASSERT_TRUE(
        second_record->settlement.finish(pipeline::RecordSettlementKind::KnownFailed, 0).ok());
    second_record.reset();
    changed.epoch_id = 3;
    changed.sample = {300, 2000, 2};
    changed.jump = rodaq::time::ClockJump{2, -200, 250};
    EXPECT_EQ(
        pipeline::submit_record(*domain_, stream_, attempt(), changed).value().disposition,
        RODAQ_SUBMIT_RESOURCE_EXHAUSTED);
    EXPECT_EQ(gate_->snapshot().accepted_records, 2);
    EXPECT_EQ(stream_.sequences.snapshot().active_remaining, 998);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    EXPECT_EQ(stream_.segment->capacity.snapshot().records, 2);
    EXPECT_EQ(stream_.segment->metadata.snapshot().accepted, 2);
    EXPECT_EQ(stream_.segment->metadata.snapshot().pending_admissions, 0);
    EXPECT_EQ(stream_.segment->metadata.snapshot().epochs, 2);
    ASSERT_TRUE(gate_->close_and_capture(instance_).ok());
    stream_.segment->close_admission();
    auto proof = stream_.segment->metadata.capture_manifest(
        {{"data.bin", 3,
          "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}});
    ASSERT_TRUE(proof.ok());
    EXPECT_EQ(proof.value().coverage[0].accepted, 2);
    EXPECT_EQ(proof.value().epochs.size(), 2);
    EXPECT_EQ(proof.value().recording_begin_ns, 0);
    EXPECT_EQ(proof.value().recording_end_ns, 100);
}

TEST_F(AdmissionTest, RecordingTimelineOverflowIsRejectedBeforeOrdinaryResourcesAndNeverWraps) {
    pipeline::AdmissionDomain overflow{
        domain_->task_id,
        domain_->pipeline_id,
        domain_->instance_id,
        domain_->generation,
        *gate_,
        *budget_,
        *pool_,
        *queue_,
        std::numeric_limits<std::uint64_t>::max(),
        100};
    auto later = received_;
    ++later.sample.monotonic_ns;
    EXPECT_EQ(
        pipeline::submit_record(overflow, stream_, attempt(), later).status().code(),
        rodaq::StatusCode::ResourceExhausted);
    expect_rolled_back();
}

TEST_F(AdmissionTest, AcceptedCopyAndActualFileTransferAllOwnershipIntoDurableLedgerCommit) {
    auto submitted = pipeline::submit_record(*domain_, stream_, attempt(), received_);
    ASSERT_TRUE(submitted.ok());
    ASSERT_EQ(submitted.value().disposition, RODAQ_SUBMIT_ACCEPTED);
    bytes_.fill(9);
    auto record = queue_->pop();
    ASSERT_TRUE(record);
    auto staging = managed_volume_->create_staging(7);
    ASSERT_TRUE(staging.ok());
    stream_.segment->capacity.mark_effects_possible();
    ASSERT_TRUE(record->settlement.mark_native_entered().ok());
    // A minimal real file sink exercises Core ownership here. Actual Plugin
    // Writer/ABI conformance and format interoperability remain separate gates.
    const auto view = record->view();
    std::ofstream file(staging.value() / "data.bin", std::ios::binary);
    file.write(reinterpret_cast<const char*>(view.payload.data), view.payload.size);
    file.close();
    ASSERT_TRUE(file);
    ASSERT_TRUE(
        record->settlement.finish(pipeline::RecordSettlementKind::Written, view.payload.size).ok());
    record.reset();
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    ASSERT_TRUE(gate_->close_and_capture(instance_).ok());
    stream_.segment->close_admission();
    rodaq::storage::SegmentSealResult seal;
    seal.files = {{"data.bin", 4}};
    seal.total_physical_bytes = 4;
    auto files = artifact::SealedFiles::open(managed_volume_, 7, seal, storage_limits_, 8192);
    ASSERT_TRUE(files.ok());
    auto manifest = stream_.segment->metadata.capture_manifest(files.value()->evidence());
    ASSERT_TRUE(manifest.ok());
    EXPECT_EQ(manifest.value().coverage[0].accepted, 1);
    EXPECT_EQ(manifest.value().coverage[0].written, 1);
    auto reservation = stream_.segment->capacity.handoff_sealed(files.value()->storage_bytes());
    ASSERT_TRUE(reservation.ok());
    auto commit = artifact::SegmentCommitter::create(
        files.value(), manifest.value(), {}, reservation.value());
    ASSERT_TRUE(commit.ok());
    stream_.segment.reset();
    EXPECT_EQ(volume_->snapshot().orphaned_reservations, 0);
    EXPECT_EQ(volume_->snapshot().commit_slots, 1);
    ASSERT_TRUE(commit.value()->commit(*ledger_).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    auto stored =
        rodaq::persistence::read_file(managed_volume_->artifacts_directory() / "7", "data.bin", 8);
    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(stored.value(), std::string("\x01\x02\x03\x04", 4));
    EXPECT_EQ(volume_->snapshot().commit_slots, 1);
    auto charge = commit.value()->take_committed_charge();
    ASSERT_TRUE(charge.ok());
    EXPECT_EQ(volume_->snapshot().commit_slots, 0);
    EXPECT_EQ(volume_->snapshot().artifact_slots, 1);
    EXPECT_EQ(volume_->snapshot().actual_bytes, 4 + commit.value()->manifest_bytes());
}

}  // namespace
