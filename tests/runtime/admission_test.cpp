#include <rodaq/runtime/admission.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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
    ResourceBudget budget(ResourceAmount{1, 8});
    AdmissionGate gate(budget);

    auto result = gate.Admit(reject_request(ResourceAmount{1, 8}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().outcome, AdmissionOutcome::GateClosed);
    EXPECT_EQ(result.value().reason, AdmissionRejectionReason::GateClosed);
    EXPECT_FALSE(result.value().reservation.Active());
    EXPECT_EQ(budget.Used(), ResourceAmount{});
}

TEST(AdmissionGateTest, AcceptAndRejectReportNormalDomainOutcomes) {
    ResourceBudget budget(ResourceAmount{1, 8});
    AdmissionGate gate(budget);
    gate.Open();

    auto accepted = gate.Admit(reject_request(ResourceAmount{1, 8}));
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted.value().outcome, AdmissionOutcome::Accepted);
    EXPECT_EQ(accepted.value().reason, AdmissionRejectionReason::None);
    EXPECT_TRUE(accepted.value().reservation.Active());

    auto rejected = gate.Admit(reject_request(ResourceAmount{1, 1}));
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(rejected.value().outcome, AdmissionOutcome::Rejected);
    EXPECT_EQ(rejected.value().reason, AdmissionRejectionReason::ResourceUnavailable);
    EXPECT_FALSE(rejected.value().reservation.Active());
}

TEST(AdmissionGateTest, WaitIsWokenByResourceRelease) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    held.value().Release();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionOutcome::Accepted);
    EXPECT_EQ(result->reservation.Amount(), (ResourceAmount{1, 4}));
}

TEST(AdmissionGateTest, WaitTimesOutWithoutAcquiringResources) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    auto result = gate.Admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(20)));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value().outcome, AdmissionOutcome::Timeout);
    EXPECT_EQ(result.value().reason, AdmissionRejectionReason::WaitTimeout);
    EXPECT_FALSE(result.value().reservation.Active());
    EXPECT_EQ(budget.ActiveReservations(), 1U);
}

TEST(AdmissionGateTest, CancellationWakesWaiter) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);
    gate.Open();
    AdmissionCancellation cancellation(budget);
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionDecision> result;
    std::thread waiter([&]() {
        result = gate.Admit(wait_request(ResourceAmount{1, 4}), cancellation).value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cancellation.Cancel();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionOutcome::Cancelled);
    EXPECT_EQ(result->reason, AdmissionRejectionReason::Cancelled);
    EXPECT_FALSE(result->reservation.Active());
}

TEST(AdmissionGateTest, GateCloseWakesWaiter) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    gate.Close();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionOutcome::GateClosed);
    EXPECT_FALSE(result->reservation.Active());
}

TEST(AdmissionGateTest, GenerationChangeRejectsAnOldWaiterAfterReopen) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);
    gate.Open();
    const std::uint64_t initial_generation = gate.Generation();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    const auto wait_deadline = Clock::now() + std::chrono::seconds(2);
    while (gate.WaitingCount() == 0 && Clock::now() < wait_deadline) {
        std::this_thread::yield();
    }
    gate.Close();
    gate.Open();
    waiter.join();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionOutcome::GateClosed);
    EXPECT_GT(gate.Generation(), initial_generation);
    EXPECT_TRUE(gate.IsOpen());
}

TEST(AdmissionGateTest, MultipleWaitersEachFinishExactlyOnce) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::atomic<int> accepted_count{0};
    std::atomic<int> closed_count{0};
    std::atomic<int> other_count{0};
    std::atomic<bool> release_success{false};
    std::vector<std::thread> waiters;
    for (int index = 0; index < 3; ++index) {
        waiters.emplace_back([&]() {
            const auto result = gate.Admit(wait_request(ResourceAmount{1, 4}));
            if (!result.has_value()) {
                other_count.fetch_add(1);
            } else if (result.value().outcome == AdmissionOutcome::Accepted) {
                accepted_count.fetch_add(1);
                while (!release_success.load()) {
                    std::this_thread::yield();
                }
            } else if (result.value().outcome == AdmissionOutcome::GateClosed) {
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

    EXPECT_EQ(accepted_count.load(), 1);
    EXPECT_EQ(closed_count.load(), 2);
    EXPECT_EQ(other_count.load(), 0);
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(AdmissionGateTest, ShutdownWithWaitersDoesNotLeaveAReservation) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);
    gate.Open();
    auto held = budget.TryReserve(ResourceAmount{1, 4});
    ASSERT_TRUE(held.has_value());

    std::optional<AdmissionDecision> result;
    std::thread waiter([&]() { result = gate.Admit(wait_request(ResourceAmount{1, 4})).value(); });
    gate.Close();
    waiter.join();
    held.value().Release();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->outcome, AdmissionOutcome::GateClosed);
    EXPECT_EQ(budget.Used(), ResourceAmount{});
    EXPECT_EQ(budget.ActiveReservations(), 0U);
}

TEST(AdmissionGateTest, ResourcesAndGateGenerationRemainConsistentUnderRaces) {
    ResourceBudget budget(ResourceAmount{1, 4});
    AdmissionGate gate(budget);

    for (int iteration = 0; iteration < 100; ++iteration) {
        gate.Open();
        std::optional<AdmissionDecision> result;
        std::thread submitter([&]() {
            result = gate.Admit(wait_request(ResourceAmount{1, 4}, std::chrono::milliseconds(50)))
                         .value();
        });
        gate.Close();
        submitter.join();

        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(
            result->outcome == AdmissionOutcome::Accepted ||
            result->outcome == AdmissionOutcome::GateClosed);
        result.reset();
        EXPECT_EQ(budget.Used(), ResourceAmount{});
        EXPECT_EQ(budget.ActiveReservations(), 0U);
    }
}

TEST(AdmissionGateTest, RejectsInvalidWaitConfigurationAndForeignCancellation) {
    ResourceBudget first_budget(ResourceAmount{1, 4});
    ResourceBudget second_budget(ResourceAmount{1, 4});
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

}  // namespace
}  // namespace rodaq
