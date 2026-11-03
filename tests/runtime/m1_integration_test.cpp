#include <rodaq/runtime/admission.hpp>
#include <rodaq/runtime/payload_pool.hpp>
#include <rodaq/runtime/registry.hpp>
#include <rodaq/runtime/resource_budget.hpp>
#include <rodaq/runtime/runtime_record.hpp>
#include <rodaq/runtime/runtime_stats.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

using Clock = std::chrono::steady_clock;

constexpr StreamHandle kStream = StreamHandle::FromRaw(100);
constexpr BindingHandle kBinding = BindingHandle::FromRaw(101);

struct RetainProbe {
    int releases = 0;
};

void release_retain_probe(void* owner_context) noexcept {
    auto* probe = static_cast<RetainProbe*>(owner_context);
    ++probe->releases;
}

TEST(M1IntegrationTest, ComposesPrimitivesAndReturnsAllResources) {
    auto registry = Registry<std::string>::Create(2);
    ASSERT_TRUE(registry.has_value());
    auto stream_metadata = registry.value().Insert("camera.front");
    ASSERT_TRUE(stream_metadata.has_value());

    auto payload_pool = PayloadPool::Create(32, 2);
    ASSERT_TRUE(payload_pool.has_value());
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{2, 64});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    auto sequences_result = AcceptanceSequenceAllocator::Create(1);
    ASSERT_TRUE(sequences_result.has_value());
    AcceptanceSequenceAllocator sequences(std::move(sequences_result.value()));
    RuntimeStats stats;
    gate.Open();

    const std::array<std::uint8_t, 6> source{0, 1, 2, 3, 4, 5};
    auto payload =
        payload_pool.value().Acquire(source.size(), PayloadView(source.data(), source.size()));
    ASSERT_TRUE(payload.has_value());

    AdmissionRequest request;
    request.resources = ResourceAmount{1, source.size()};
    request.mode = AdmissionMode::Reject;
    auto admission = gate.Admit(request);
    ASSERT_TRUE(admission.has_value());
    ASSERT_EQ(admission.value().outcome, AdmissionGateOutcome::Granted);

    auto sequence = sequences.Allocate(kStream);
    ASSERT_TRUE(sequence.has_value());
    TimestampState timestamps;
    ASSERT_TRUE(timestamps.Set(0, 1234).has_value());
    {
        auto record = RuntimeRecord::Create(
            kBinding, sequence.value(), std::nullopt, std::nullopt, timestamps,
            std::move(payload.value()), std::move(admission.value().reservation));
        ASSERT_TRUE(record.has_value());
        stats.RecordAccepted(record.value().RecordPayload().Size());
        stats.RecordInflightAcquired(record.value().RecordPayload().Size());

        EXPECT_NE(registry.value().Find(stream_metadata.value()), nullptr);
        EXPECT_EQ(*registry.value().Find(stream_metadata.value()), "camera.front");
        EXPECT_EQ(payload_pool.value().UsedBytes(), 32U);
        EXPECT_EQ(budget.Used(), (ResourceAmount{1, 6}));
        const RuntimeStatsSnapshot inflight = stats.Snapshot();
        EXPECT_EQ(inflight.accepted_records, 1U);
        EXPECT_EQ(inflight.inflight_records, 1U);
        stats.RecordInflightReleased(record.value().RecordPayload().Size());
    }

    EXPECT_EQ(payload_pool.value().UsedBytes(), 0U);
    EXPECT_EQ(payload_pool.value().AvailableBytes(), payload_pool.value().CapacityBytes());
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
    EXPECT_EQ(registry.value().Size(), 1U);

    const RuntimeStatsSnapshot final = stats.Snapshot();
    EXPECT_EQ(final.accepted_records, 1U);
    EXPECT_EQ(final.accepted_bytes, source.size());
    EXPECT_EQ(final.inflight_records, 0U);
    EXPECT_EQ(final.inflight_bytes, 0U);
    EXPECT_GE(final.high_water_records, 1U);
    EXPECT_GE(final.high_water_bytes, source.size());
}

TEST(M1IntegrationTest, ResourceUnavailableDoesNotConsumeAcceptanceSequence) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    auto sequences_result = AcceptanceSequenceAllocator::Create(1);
    ASSERT_TRUE(sequences_result.has_value());
    AcceptanceSequenceAllocator sequences(std::move(sequences_result.value()));
    gate.Open();

    auto holder = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(holder.has_value());

    AdmissionRequest request;
    request.resources = ResourceAmount{1, 4};
    request.mode = AdmissionMode::Reject;
    auto unavailable = gate.Admit(request);
    ASSERT_TRUE(unavailable.has_value());
    EXPECT_EQ(unavailable.value().outcome, AdmissionGateOutcome::ResourceUnavailable);

    holder.value().Release();
    auto accepted = gate.Admit(request);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_EQ(accepted.value().outcome, AdmissionGateOutcome::Granted);
    auto first_sequence = sequences.Allocate(kStream);
    ASSERT_TRUE(first_sequence.has_value());
    EXPECT_EQ(first_sequence.value(), 0U);
}

TEST(M1IntegrationTest, RetainedBytesRemainBudgetedUntilRecordDestruction) {
    const std::array<std::uint8_t, 4> source{1, 2, 3, 4};
    RetainProbe probe;
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, source.size()});
    ResourceBudget& budget = *budget_owner;
    auto reservation = budget.TryReserve(ResourceAmount{1, source.size()});
    ASSERT_TRUE(reservation.has_value());
    auto retained =
        Payload::Retain(PayloadView(source.data(), source.size()), &probe, &release_retain_probe);
    ASSERT_TRUE(retained.has_value());

    {
        auto record = RuntimeRecord::Create(
            kBinding, 0, std::nullopt, std::nullopt, TimestampState{}, std::move(retained.value()),
            std::move(reservation.value()));
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ(budget.Used(), (ResourceAmount{1, source.size()}));
        EXPECT_EQ(probe.releases, 0);
    }

    EXPECT_EQ(probe.releases, 1);
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(M1IntegrationTest, BoundedMultiProducerRejectWaitMixLeavesNoLeaks) {
    constexpr int kThreadCount = 4;
    constexpr int kOperationsPerThread = 120;
    constexpr std::size_t kPayloadSize = 8;
    constexpr int kExpectedOperations = kThreadCount * kOperationsPerThread;

    auto payload_pool = PayloadPool::Create(16, kThreadCount * 4);
    ASSERT_TRUE(payload_pool.has_value());
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{2, kPayloadSize * 2});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    auto sequences_result = AcceptanceSequenceAllocator::Create(1);
    ASSERT_TRUE(sequences_result.has_value());
    AcceptanceSequenceAllocator sequences(std::move(sequences_result.value()));
    RuntimeStats stats;
    gate.Open();

    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    std::atomic<int> timed_out{0};
    std::atomic<int> failures{0};
    std::vector<std::uint64_t> observed_sequences;
    observed_sequences.reserve(kExpectedOperations);
    std::mutex sequences_mutex;
    const std::array<std::uint8_t, kPayloadSize> source{0, 1, 2, 3, 4, 5, 6, 7};

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int producer = 0; producer < kThreadCount; ++producer) {
        producers.emplace_back([&, producer]() {
            for (int operation = 0; operation < kOperationsPerThread; ++operation) {
                auto payload = payload_pool.value().Acquire(
                    kPayloadSize, PayloadView(source.data(), source.size()));
                if (!payload.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }

                AdmissionRequest request;
                request.resources = ResourceAmount{1, kPayloadSize};
                request.mode =
                    (producer + operation) % 2 == 0 ? AdmissionMode::Reject : AdmissionMode::Wait;
                if (request.mode == AdmissionMode::Wait) {
                    request.deadline = Clock::now() + std::chrono::milliseconds(500);
                }
                auto admission = gate.Admit(request);
                if (!admission.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                if (admission.value().outcome == AdmissionGateOutcome::ResourceUnavailable) {
                    rejected.fetch_add(1);
                    stats.RecordRejected();
                    continue;
                }
                if (admission.value().outcome == AdmissionGateOutcome::Timeout) {
                    timed_out.fetch_add(1);
                    stats.RecordTimedOut();
                    continue;
                }
                if (admission.value().outcome != AdmissionGateOutcome::Granted) {
                    failures.fetch_add(1);
                    continue;
                }

                auto sequence = sequences.Allocate(kStream);
                if (!sequence.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(sequences_mutex);
                    observed_sequences.push_back(sequence.value());
                }
                auto record = RuntimeRecord::Create(
                    kBinding, sequence.value(), std::nullopt, std::nullopt, TimestampState{},
                    std::move(payload.value()), std::move(admission.value().reservation));
                if (!record.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                stats.RecordAccepted(kPayloadSize);
                stats.RecordInflightAcquired(kPayloadSize);
                std::this_thread::yield();
                stats.RecordInflightReleased(kPayloadSize);
                accepted.fetch_add(1);
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }
    gate.Close();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(accepted.load() + rejected.load() + timed_out.load(), kExpectedOperations);
    EXPECT_GT(accepted.load(), 0);
    EXPECT_GT(rejected.load(), 0);
    EXPECT_EQ(payload_pool.value().UsedBytes(), 0U);
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
    EXPECT_EQ(gate.WaitingCount(), 0U);

    std::sort(observed_sequences.begin(), observed_sequences.end());
    EXPECT_EQ(
        std::adjacent_find(observed_sequences.begin(), observed_sequences.end()),
        observed_sequences.end());

    const RuntimeStatsSnapshot snapshot = stats.Snapshot();
    EXPECT_EQ(snapshot.accepted_records, static_cast<std::uint64_t>(accepted.load()));
    EXPECT_EQ(snapshot.rejected_records, static_cast<std::uint64_t>(rejected.load()));
    EXPECT_EQ(snapshot.timed_out_records, static_cast<std::uint64_t>(timed_out.load()));
    EXPECT_EQ(snapshot.inflight_records, 0U);
    EXPECT_EQ(snapshot.inflight_bytes, 0U);
    EXPECT_GE(snapshot.high_water_records, 1U);
    EXPECT_GE(snapshot.high_water_bytes, kPayloadSize);
}

}  // namespace
}  // namespace rodaq
