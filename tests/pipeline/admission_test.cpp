#include "pipeline/admission.hpp"
#include "pipeline/descriptor_registry.hpp"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <future>
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
        auto ledger =
            artifact::LedgerStore::open(ledger_root_, artifact::LedgerOpenMode::Initialize, 4);
        ASSERT_TRUE(ledger.ok());
        ledger_ = std::move(ledger.value());
        auto volume = artifact::VolumeCapacity::create({1000000, 1000000, 100, 1000, 8, 8, 16});
        ASSERT_TRUE(volume.ok());
        volume_.emplace(std::move(volume.value()));
        auto probe = volume_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(volume_->finish_space_sample(probe.value(), 1000000, 100).ok());
        rodaq::storage::PrepareConfig limits{};
        limits.max_record_bytes = 8;
        limits.max_segment_records = 1000;
        limits.max_segment_input_bytes = 8000;
        limits.max_segment_physical_bytes = 100000;
        limits.max_descriptors_per_segment = 8;
        rodaq::storage::CapacityModel model{};
        model.payload_expansion_numerator = 1;
        model.payload_expansion_denominator = 1;
        model.per_record_overhead_bytes = 10;
        model.descriptor_expansion_numerator = 1;
        model.descriptor_expansion_denominator = 1;
        model.fixed_segment_overhead_bytes = 50;
        model.seal_reserve_bytes = 50;
        auto registration = ledger_->reserve_registration(7);
        ASSERT_TRUE(registration.ok());
        auto segment = pipeline::SegmentCapacity::create(
            7, limits, model, 100, *volume_, std::move(registration.value()), 100);
        ASSERT_TRUE(segment.ok());
        stream_.segment = std::make_shared<pipeline::SegmentCapacity>(std::move(segment.value()));
        auto registry = pipeline::DescriptorRegistry::create({1});
        ASSERT_TRUE(registry.ok());
        registry_.emplace(std::move(registry.value()));
        stream_.active_descriptor = descriptor(1);
        ASSERT_TRUE(stream_.active_descriptor);
        ASSERT_TRUE(stream_.sequences.begin_refill(1).ok());
        ASSERT_TRUE(stream_.sequences.finish_refill(1, {1, 1001}).ok());
        budget_.emplace(std::move(pipeline::RecordBudget::create(16, 128).value()));
        pool_.emplace(std::move(pipeline::PayloadPool::create({8192, 256, 8, 16}).value()));
        queue_.emplace(std::move(pipeline::RecordQueue::create(8).value()));
        instance_.value[15] = 1;
        gate_.emplace(instance_, 1);
        ASSERT_EQ(gate_->open_admission(instance_, 1), pipeline::GateOpenResult::Opened);
        const auto task = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        domain_.emplace(
            pipeline::AdmissionDomain{task, 1, instance_, 1, *gate_, *budget_, *pool_, *queue_});
        received_.boot_id = 1;
        received_.epoch_id = 1;
        received_.sample = {100, 1000, 1};
    }

    void TearDown() override {
        stream_.segment.reset();
        ledger_.reset();
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
        EXPECT_EQ(stream_.segment->snapshot().records, 0);
        EXPECT_EQ(stream_.segment->snapshot().candidates, 0);
        EXPECT_EQ(stream_.sequences.snapshot().active_remaining, 1000);
        EXPECT_EQ(volume_->snapshot().future_bytes, 200);
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
    EXPECT_EQ(volume_->snapshot().future_bytes, 214);
    EXPECT_EQ(gate_->snapshot().accepted_records, 1);
    record.reset();
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    EXPECT_EQ(stream_.segment->snapshot().records, 1);
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
    EXPECT_EQ(stream_.segment->snapshot().records, 0);
    EXPECT_EQ(volume_->snapshot().future_bytes, 200);
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
    EXPECT_EQ(stream_.sequences.snapshot().active_remaining, 600);
    EXPECT_EQ(budget_->snapshot().records, 0);
    EXPECT_EQ(pool_->snapshot().live_buffers, 0);
    EXPECT_EQ(queue_->snapshot().reserved_slots, 0);
    EXPECT_EQ(stream_.segment->snapshot().candidates, 0);
    EXPECT_EQ(stream_.segment->snapshot().records, 400);
    EXPECT_EQ(volume_->snapshot().future_bytes, 5800);
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
    EXPECT_EQ(stream_.segment->snapshot().records, 1);
    EXPECT_EQ(volume_->snapshot().future_bytes, 214);
    EXPECT_EQ(stream_.sequences.snapshot().active_remaining, 999);
}
#endif

}  // namespace
