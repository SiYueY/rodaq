#include <rodaq/runtime/admission.hpp>
#include <rodaq/runtime/runtime_record.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace rodaq {
namespace {

using Clock = std::chrono::steady_clock;

AdmissionRequest reject_request(ResourceAmount resources) {
    AdmissionRequest request;
    request.resources = resources;
    request.mode = AdmissionMode::Reject;
    return request;
}

AdmissionRequest wait_request(
    ResourceAmount resources, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    AdmissionRequest request;
    request.resources = resources;
    request.mode = AdmissionMode::Wait;
    request.deadline = Clock::now() + timeout;
    return request;
}

TEST(AdmissionGateTest, ClosedGateDoesNotReserveResources) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 8});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);

    auto result = gate.Admit(reject_request(ResourceAmount{1, 8}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_FALSE(result.value().reservation.Active());
    EXPECT_EQ(budget.Used(), ResourceAmount{});
}

TEST(AdmissionGateTest, ReportsGrantOrResourceUnavailable) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 8});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();

    auto granted = gate.Admit(reject_request(ResourceAmount{1, 8}));
    ASSERT_TRUE(granted.has_value());
    EXPECT_EQ(granted.value().outcome, AdmissionGateOutcome::Granted);
    EXPECT_TRUE(granted.value().reservation.Active());

    auto unavailable = gate.Admit(reject_request(ResourceAmount{1, 1}));
    ASSERT_TRUE(unavailable.has_value());
    EXPECT_EQ(unavailable.value().outcome, AdmissionGateOutcome::ResourceUnavailable);
    EXPECT_FALSE(unavailable.value().reservation.Active());
}

TEST(AdmissionGateTest, WaitIsWokenByResourceRelease) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    held.value().Release();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::Granted);
    EXPECT_EQ(result->reservation.Amount(), (ResourceAmount{1, 4}));
}

TEST(AdmissionGateTest, WaitTimesOutWithoutAcquiringResources) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    auto result = gate.Admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(20)));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().outcome, AdmissionGateOutcome::Timeout);
    EXPECT_FALSE(result.value().reservation.Active());
    EXPECT_EQ(budget.ActiveReservations(), 1U);
}

TEST(AdmissionGateTest, CancellationWakesWaiter) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();
    AdmissionCancellation cancellation(budget);
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() {
        result = gate.Admit(wait_request(ResourceAmount{1, 4}), cancellation).value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancellation.Cancel();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::Cancelled);
    EXPECT_FALSE(result->reservation.Active());
}

TEST(AdmissionGateTest, GateCloseWakesWaiter) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    gate.Close();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_FALSE(result->reservation.Active());
}

TEST(AdmissionGateTest, GenerationChangeRejectsAnOldWaiterAfterReopen) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();
    const std::uint64_t initial_generation = gate.Generation();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    const auto wait_deadline = Clock::now() + std::chrono::seconds(2);
    while (gate.WaitingCount() == 0 && Clock::now() < wait_deadline) {
        std::this_thread::yield();
    }
    gate.Close();
    gate.Open();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_GT(gate.Generation(), initial_generation);
    EXPECT_TRUE(gate.IsOpen());
}

TEST(AdmissionGateTest, MultipleWaitersEachFinishExactlyOnce) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::atomic<int> granted_count{0};
    std::atomic<int> closed_count{0};
    std::atomic<int> other_count{0};
    std::atomic<bool> release_success{false};
    std::vector<std::thread> waiters;
    for (int index = 0; index < 3; ++index) {
        waiters.emplace_back([&]() {
            const auto result = gate.Admit(wait_request(ResourceAmount{1, 4}));
            if (!result.has_value()) {
                other_count.fetch_add(1);
            } else if (result.value().outcome == AdmissionGateOutcome::Granted) {
                granted_count.fetch_add(1);
                while (!release_success.load()) {
                    std::this_thread::yield();
                }
            } else if (result.value().outcome == AdmissionGateOutcome::GateClosed) {
                closed_count.fetch_add(1);
            } else {
                other_count.fetch_add(1);
            }
        });
    }

    while (gate.WaitingCount() != 3) {
        std::this_thread::yield();
    }
    held.value().Release();
    const auto wait_deadline = Clock::now() + std::chrono::seconds(2);
    while (budget.ActiveReservations() == 0 && Clock::now() < wait_deadline) {
        std::this_thread::yield();
    }
    gate.Close();
    release_success.store(true);
    for (auto& waiter : waiters) {
        waiter.join();
    }

    EXPECT_EQ(granted_count.load(), 1);
    EXPECT_EQ(closed_count.load(), 2);
    EXPECT_EQ(other_count.load(), 0);
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(AdmissionGateTest, ShutdownWithWaitersDoesNotLeaveAReservation) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    gate.Close();
    waiter.join();
    held.value().Release();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(AdmissionGateTest, ResourcesAndGateGenerationRemainConsistentUnderRaces) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);

    for (int iteration = 0; iteration < 100; ++iteration) {
        gate.Open();
        std::optional<AdmissionGateDecision> result;
        std::thread submitter([&]() {
            result = gate.Admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(50)))
                         .value();
        });
        gate.Close();
        submitter.join();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(
            result->outcome == AdmissionGateOutcome::Granted ||
            result->outcome == AdmissionGateOutcome::GateClosed);
        result.reset();
        EXPECT_EQ(budget.Used(), ResourceAmount{});
        EXPECT_EQ(budget.ActiveReservations(), 0U);
    }
}

TEST(AdmissionGateTest, ResourceReleaseAndDeadlineRaceFinishesExactlyOnce) {
    for (int iteration = 0; iteration < 50; ++iteration) {
        auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
        ResourceBudget& budget = *budget_owner;
        AdmissionGate gate(budget);
        gate.Open();
        auto held = budget.TryReserve(ResourceAmount{1, 4});
        ASSERT_TRUE(held.has_value());

        std::optional<AdmissionGateDecision> result;
        std::thread waiter([&]() {
            result = gate.Admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(100)))
                         .value();
        });
        held.value().Release();
        waiter.join();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(
            result->outcome == AdmissionGateOutcome::Granted ||
            result->outcome == AdmissionGateOutcome::Timeout);
        result.reset();
        EXPECT_EQ(budget.Used(), ResourceAmount{});
        EXPECT_EQ(budget.ActiveReservations(), 0U);
    }
}

TEST(AdmissionGateTest, ResourceReleaseAndCancellationRaceFinishesExactlyOnce) {
    for (int iteration = 0; iteration < 50; ++iteration) {
        auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
        ResourceBudget& budget = *budget_owner;
        AdmissionGate gate(budget);
        AdmissionCancellation cancellation(budget);
        gate.Open();
        auto held = budget.TryReserve(ResourceAmount{1, 4});
        ASSERT_TRUE(held.has_value());

        std::optional<AdmissionGateDecision> result;
        std::thread waiter([&]() {
            result = gate.Admit(wait_request(ResourceAmount{1, 4}), cancellation).value();
        });
        held.value().Release();
        cancellation.Cancel();
        waiter.join();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(
            result->outcome == AdmissionGateOutcome::Granted ||
            result->outcome == AdmissionGateOutcome::Cancelled);
        result.reset();
        EXPECT_EQ(budget.Used(), ResourceAmount{});
        EXPECT_EQ(budget.ActiveReservations(), 0U);
    }
}

TEST(AdmissionGateTest, RejectsInvalidWaitConfigurationAndForeignCancellation) {
    auto first_budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& first_budget = *first_budget_owner;
    auto second_budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& second_budget = *second_budget_owner;
    AdmissionGate gate(first_budget);
    gate.Open();

    AdmissionRequest no_deadline;
    no_deadline.mode = AdmissionMode::Wait;
    const auto invalid = gate.Admit(no_deadline);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);

    AdmissionCancellation foreign_cancellation(second_budget);
    const auto wrong_budget =
        gate.Admit(reject_request(ResourceAmount{1, 4}), foreign_cancellation);
    ASSERT_FALSE(wrong_budget.has_value());
    EXPECT_EQ(wrong_budget.error().code(), ErrorCode::InvalidArgument);
}

TEST(AdmissionGateTest, GrantDoesNotAllocateAcceptanceSequence) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    auto sequences_result = AcceptanceSequenceAllocator::Create(1);
    ASSERT_TRUE(sequences_result.has_value());
    AcceptanceSequenceAllocator sequences(std::move(sequences_result.value()));
    gate.Open();

    const auto granted = gate.Admit(reject_request(ResourceAmount{1, 4}));
    ASSERT_TRUE(granted.has_value());
    ASSERT_EQ(granted.value().outcome, AdmissionGateOutcome::Granted);

    const auto first_sequence = sequences.Allocate(StreamHandle::FromRaw(1));
    ASSERT_TRUE(first_sequence.has_value());
    EXPECT_EQ(first_sequence.value(), 0U);
}

TEST(AdmissionGateTest, GenerationExhaustionIsTerminalAndDoesNotWrap) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    constexpr std::uint64_t kTerminal = std::numeric_limits<std::uint64_t>::max();
    AdmissionGate gate(budget, kTerminal - 2U);

    gate.Open();
    EXPECT_TRUE(gate.IsOpen());
    EXPECT_EQ(gate.Generation(), kTerminal - 1U);
    gate.Close();
    EXPECT_FALSE(gate.IsOpen());
    EXPECT_EQ(gate.Generation(), kTerminal);

    gate.Open();
    EXPECT_FALSE(gate.IsOpen());
    EXPECT_EQ(gate.Generation(), kTerminal);

    const auto closed = gate.Admit(reject_request(ResourceAmount{1, 4}));
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed.value().outcome, AdmissionGateOutcome::GateClosed);
}

}  // namespace
}  // namespace rodaq
