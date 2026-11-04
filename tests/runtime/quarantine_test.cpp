#include "runtime/quarantine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace rodaq::runtime {
namespace {

TEST(QuarantineManagerTest, CompletesAndReapsSupervisedCalls) {
    auto& manager = QuarantineManager::Process();
    const std::size_t before = manager.RetainedCallCount();
    auto outcome = manager.Run(
        [] { return Result<void>(); }, std::chrono::steady_clock::now() + std::chrono::seconds(1),
        "completed call");
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().status, SupervisedCallStatus::Completed);
    EXPECT_EQ(manager.RetainedCallCount(), before);
}

TEST(QuarantineManagerTest, RetainsThenJoinsTimedOutCall) {
    auto& manager = QuarantineManager::Process();
    const std::size_t before = manager.RetainedCallCount();
    auto gate = std::make_shared<std::atomic<bool>>(false);
    auto outcome = manager.Run(
        [gate] {
            while (!gate->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Result<void>();
        },
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1), "timed out call");
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome.value().status, SupervisedCallStatus::TimedOut);
    EXPECT_GT(manager.RetainedCallCount(), before);

    gate->store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (manager.RetainedCallCount() != before && std::chrono::steady_clock::now() < deadline) {
        manager.ReapCompleted();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(manager.RetainedCallCount(), before);
}

TEST(QuarantineManagerTest, BoundsActivationRootsAndReleasesCleanSlots) {
    auto& manager = QuarantineManager::Process();
    std::vector<std::uint64_t> slots;
    while (true) {
        auto slot = manager.ReserveActivation("activation slot test");
        if (!slot.has_value()) {
            EXPECT_EQ(slot.error().code(), ErrorCode::ResourceExhausted);
            break;
        }
        slots.push_back(slot.value());
        ASSERT_LE(slots.size(), 64U);
    }
    EXPECT_FALSE(slots.empty());

    auto root = std::make_shared<int>(42);
    ASSERT_TRUE(manager.RetainActivation(slots.front(), root, "retained root test"));
    for (std::size_t index = 1; index < slots.size(); ++index) {
        manager.ReleaseActivation(slots[index]);
    }
    EXPECT_GE(manager.RetainedActivationCount(), 1U);
}

}  // namespace
}  // namespace rodaq::runtime
