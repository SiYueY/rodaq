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

    auto result = gate.admit(reject_request(ResourceAmount{1, 8}));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_FALSE(result.value().reservation.active());
    EXPECT_EQ(budget.used(), ResourceAmount{});
}

TEST(AdmissionGateTest, ReportsGrantOrResourceUnavailable) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 8});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();

    auto granted = gate.admit(reject_request(ResourceAmount{1, 8}));
    ASSERT_TRUE(granted.ok());
    EXPECT_EQ(granted.value().outcome, AdmissionGateOutcome::Granted);
    EXPECT_TRUE(granted.value().reservation.active());

    auto unavailable = gate.admit(reject_request(ResourceAmount{1, 1}));
    ASSERT_TRUE(unavailable.ok());
    EXPECT_EQ(unavailable.value().outcome, AdmissionGateOutcome::ResourceUnavailable);
    EXPECT_FALSE(unavailable.value().reservation.active());
}

TEST(AdmissionGateTest, WaitIsWokenByResourceRelease) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();
    auto held = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.ok());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.admit(wait_request(ResourceAmount{1, 4})).value(); });
    held.value().release();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::Granted);
    EXPECT_EQ(result->reservation.amount(), (ResourceAmount{1, 4}));
}

TEST(AdmissionGateTest, WaitTimesOutWithoutAcquiringResources) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();
    auto held = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.ok());

    auto result = gate.admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(20)));
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().outcome, AdmissionGateOutcome::Timeout);
    EXPECT_FALSE(result.value().reservation.active());
    EXPECT_EQ(budget.active_reservations(), 1U);
}

TEST(AdmissionGateTest, CancellationWakesWaiter) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();
    AdmissionCancellation cancellation(budget);
    auto held = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.ok());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() {
        result = gate.admit(wait_request(ResourceAmount{1, 4}), cancellation).value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancellation.cancel();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::Cancelled);
    EXPECT_FALSE(result->reservation.active());
}

TEST(AdmissionGateTest, GateCloseWakesWaiter) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();
    auto held = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.ok());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.admit(wait_request(ResourceAmount{1, 4})).value(); });
    gate.close();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_FALSE(result->reservation.active());
}

TEST(AdmissionGateTest, GenerationChangeRejectsAnOldWaiterAfterReopen) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();
    const std::uint64_t initial_generation = gate.generation();
    auto held = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.ok());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.admit(wait_request(ResourceAmount{1, 4})).value(); });
    const auto wait_deadline = Clock::now() + std::chrono::seconds(2);
    while (gate.waiting_count() == 0 && Clock::now() < wait_deadline) {
        std::this_thread::yield();
    }
    gate.close();
    gate.open();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_GT(gate.generation(), initial_generation);
    EXPECT_TRUE(gate.is_open());
}

TEST(AdmissionGateTest, MultipleWaitersEachFinishExactlyOnce) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();
    auto held = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.ok());

    std::atomic<int> granted_count{0};
    std::atomic<int> closed_count{0};
    std::atomic<int> other_count{0};
    std::atomic<bool> release_success{false};
    std::vector<std::thread> waiters;
    for (int index = 0; index < 3; ++index) {
        waiters.emplace_back([&]() {
            const auto result = gate.admit(wait_request(ResourceAmount{1, 4}));
            if (!result.ok()) {
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

    while (gate.waiting_count() != 3) {
        std::this_thread::yield();
    }
    held.value().release();
    const auto wait_deadline = Clock::now() + std::chrono::seconds(2);
    while (budget.active_reservations() == 0 && Clock::now() < wait_deadline) {
        std::this_thread::yield();
    }
    gate.close();
    release_success.store(true);
    for (auto& waiter : waiters) {
        waiter.join();
    }

    EXPECT_EQ(granted_count.load(), 1);
    EXPECT_EQ(closed_count.load(), 2);
    EXPECT_EQ(other_count.load(), 0);
    EXPECT_EQ(budget.active_reservations(), 0U);
}

TEST(AdmissionGateTest, ShutdownWithWaitersDoesNotLeaveAReservation) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    gate.open();
    auto held = budget.try_reserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.ok());

    std::optional<AdmissionGateDecision> result;
    std::thread waiter([&]() { result = gate.admit(wait_request(ResourceAmount{1, 4})).value(); });
    gate.close();
    waiter.join();
    held.value().release();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionGateOutcome::GateClosed);
    EXPECT_EQ(budget.used(), ResourceAmount{});
    EXPECT_EQ(budget.active_reservations(), 0U);
}

TEST(AdmissionGateTest, ResourcesAndGateGenerationRemainConsistentUnderRaces) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);

    for (int iteration = 0; iteration < 100; ++iteration) {
        gate.open();
        std::optional<AdmissionGateDecision> result;
        std::thread submitter([&]() {
            result = gate.admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(50)))
                         .value();
        });
        gate.close();
        submitter.join();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(
            result->outcome == AdmissionGateOutcome::Granted ||
            result->outcome == AdmissionGateOutcome::GateClosed);
        result.reset();
        EXPECT_EQ(budget.used(), ResourceAmount{});
        EXPECT_EQ(budget.active_reservations(), 0U);
    }
}

TEST(AdmissionGateTest, ResourceReleaseAndDeadlineRaceFinishesExactlyOnce) {
    for (int iteration = 0; iteration < 50; ++iteration) {
        auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
        ResourceBudget& budget = *budget_owner;
        AdmissionGate gate(budget);
        gate.open();
        auto held = budget.try_reserve(ResourceAmount{1, 4});
        ASSERT_TRUE(held.ok());

        std::optional<AdmissionGateDecision> result;
        std::thread waiter([&]() {
            result = gate.admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(100)))
                         .value();
        });
        held.value().release();
        waiter.join();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(
            result->outcome == AdmissionGateOutcome::Granted ||
            result->outcome == AdmissionGateOutcome::Timeout);
        result.reset();
        EXPECT_EQ(budget.used(), ResourceAmount{});
        EXPECT_EQ(budget.active_reservations(), 0U);
    }
}

TEST(AdmissionGateTest, ResourceReleaseAndCancellationRaceFinishesExactlyOnce) {
    for (int iteration = 0; iteration < 50; ++iteration) {
        auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
        ResourceBudget& budget = *budget_owner;
        AdmissionGate gate(budget);
        AdmissionCancellation cancellation(budget);
        gate.open();
        auto held = budget.try_reserve(ResourceAmount{1, 4});
        ASSERT_TRUE(held.ok());

        std::optional<AdmissionGateDecision> result;
        std::thread waiter([&]() {
            result = gate.admit(wait_request(ResourceAmount{1, 4}), cancellation).value();
        });
        held.value().release();
        cancellation.cancel();
        waiter.join();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(
            result->outcome == AdmissionGateOutcome::Granted ||
            result->outcome == AdmissionGateOutcome::Cancelled);
        result.reset();
        EXPECT_EQ(budget.used(), ResourceAmount{});
        EXPECT_EQ(budget.active_reservations(), 0U);
    }
}

TEST(AdmissionGateTest, RejectsInvalidWaitConfigurationAndForeignCancellation) {
    auto first_budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& first_budget = *first_budget_owner;
    auto second_budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& second_budget = *second_budget_owner;
    AdmissionGate gate(first_budget);
    gate.open();

    AdmissionRequest no_deadline;
    no_deadline.mode = AdmissionMode::Wait;
    const auto invalid = gate.admit(no_deadline);
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().status_code(), StatusCode::InvalidArgument);

    AdmissionCancellation foreign_cancellation(second_budget);
    const auto wrong_budget =
        gate.admit(reject_request(ResourceAmount{1, 4}), foreign_cancellation);
    ASSERT_FALSE(wrong_budget.ok());
    EXPECT_EQ(wrong_budget.status().status_code(), StatusCode::InvalidArgument);
}

TEST(AdmissionGateTest, GrantDoesNotAllocateAcceptanceSequence) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    AdmissionGate gate(budget);
    auto sequences_result = AcceptanceSequenceAllocator::create(1);
    ASSERT_TRUE(sequences_result.ok());
    AcceptanceSequenceAllocator sequences(std::move(sequences_result.value()));
    gate.open();

    const auto granted = gate.admit(reject_request(ResourceAmount{1, 4}));
    ASSERT_TRUE(granted.ok());
    ASSERT_EQ(granted.value().outcome, AdmissionGateOutcome::Granted);

    const auto first_sequence = sequences.allocate(StreamHandle::from_raw(1));
    ASSERT_TRUE(first_sequence.ok());
    EXPECT_EQ(first_sequence.value(), 0U);
}

TEST(AdmissionGateTest, GenerationExhaustionIsTerminalAndDoesNotWrap) {
    auto budget_owner = std::make_unique<ResourceBudget>(ResourceAmount{1, 4});
    ResourceBudget& budget = *budget_owner;
    constexpr std::uint64_t kTerminal = std::numeric_limits<std::uint64_t>::max();
    AdmissionGate gate(budget, kTerminal - 2U);

    gate.open();
    EXPECT_TRUE(gate.is_open());
    EXPECT_EQ(gate.generation(), kTerminal - 1U);
    gate.close();
    EXPECT_FALSE(gate.is_open());
    EXPECT_EQ(gate.generation(), kTerminal);

    gate.open();
    EXPECT_FALSE(gate.is_open());
    EXPECT_EQ(gate.generation(), kTerminal);

    const auto closed = gate.admit(reject_request(ResourceAmount{1, 4}));
    ASSERT_TRUE(closed.ok());
    EXPECT_EQ(closed.value().outcome, AdmissionGateOutcome::GateClosed);
}

}  // namespace
}  // namespace rodaq
