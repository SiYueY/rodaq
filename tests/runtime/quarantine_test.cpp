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

TEST(QuarantineManagerTest, ConcurrentReapersJoinOnceAndReleaseStateOutsideManagerLock) {
    auto& manager = QuarantineManager::Process();
    manager.ReapCompleted();
    const auto before = manager.RetainedWorkerCount();
    auto released = std::make_shared<std::atomic<bool>>(false);
    auto destroyed = std::make_shared<std::atomic<int>>(0);
    struct WorkerState {
        std::shared_ptr<std::atomic<int>> destroyed;
        ~WorkerState() {
            // Capture destruction may call Runtime infrastructure after Join.
            static_cast<void>(QuarantineManager::Process().RetainedCallCount());
            destroyed->fetch_add(1);
        }
    };
    constexpr int kWorkers = 16;
    for (int index = 0; index < kWorkers; ++index) {
        auto state = std::make_shared<WorkerState>();
        state->destroyed = destroyed;
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread worker([released, done, state, kWorkers] {
            while (!released->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            done->store(true, std::memory_order_release);
            // A publication of completion precedes thread exit. Join must still
            // protect this final use of the retained capture.
            EXPECT_LT(state->destroyed->load(), kWorkers);
        });
        ASSERT_TRUE(manager.RetainWorker(std::move(worker), state, done, "reaper race"));
    }
    EXPECT_EQ(manager.RetainedWorkerCount(), before + kWorkers);
    auto reap = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (manager.RetainedWorkerCount() != before &&
               std::chrono::steady_clock::now() < deadline) {
            manager.ReapCompleted();
            std::this_thread::yield();
        }
    };
    std::thread first(reap);
    std::thread second(reap);
    released->store(true, std::memory_order_release);
    first.join();
    second.join();
    manager.ReapCompleted();
    manager.ReapCompleted();
    EXPECT_EQ(manager.RetainedWorkerCount(), before);
    EXPECT_EQ(destroyed->load(), kWorkers);
}

TEST(QuarantineManagerTest, CompletedWorkerMayReenterReapingWithoutJoiningItself) {
    auto& manager = QuarantineManager::Process();
    const auto before = manager.RetainedWorkerCount();
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto returned = std::make_shared<std::atomic<bool>>(false);
    auto state = std::make_shared<int>(7);
    std::thread worker([&manager, entered, done, returned] {
        while (!entered->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        done->store(true, std::memory_order_release);
        manager.ReapCompleted();
        returned->store(true, std::memory_order_release);
    });
    ASSERT_TRUE(manager.RetainWorker(std::move(worker), state, done, "self-reap test"));
    entered->store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (manager.RetainedWorkerCount() != before && std::chrono::steady_clock::now() < deadline) {
        manager.ReapCompleted();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(manager.RetainedWorkerCount(), before);
    EXPECT_TRUE(returned->load(std::memory_order_acquire));
}

TEST(QuarantineManagerTest, CaptureDestructionCompletesBeforeJoinPublication) {
    auto& manager = QuarantineManager::Process();
    const auto before = manager.RetainedCallCount();
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto joined = std::make_shared<std::atomic<bool>>(false);
    struct Capture {
        std::shared_ptr<std::atomic<bool>> release;
        std::shared_ptr<std::atomic<bool>> entered;
        ~Capture() {
            entered->store(true, std::memory_order_release);
            while (!release->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    };
    struct ReleaseGuard {
        std::shared_ptr<std::atomic<bool>> release;
        ~ReleaseGuard() { release->store(true, std::memory_order_release); }
    } guard{release};
    auto capture = std::make_shared<Capture>();
    capture->release = release;
    capture->entered = entered;
    std::function<Result<void>()> operation = [capture] { return Result<void>(); };
    capture.reset();
    auto outcome = manager.Run(
        std::move(operation), std::chrono::steady_clock::now() + std::chrono::milliseconds(10),
        "blocking capture destruction", joined);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->status, SupervisedCallStatus::TimedOut);
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered->load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(entered->load(std::memory_order_acquire));
    EXPECT_FALSE(joined->load(std::memory_order_acquire));
    release->store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!joined->load(std::memory_order_acquire) || manager.RetainedCallCount() != before) &&
           std::chrono::steady_clock::now() < deadline) {
        manager.ReapCompleted();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(joined->load(std::memory_order_acquire));
    EXPECT_EQ(manager.RetainedCallCount(), before);
}

TEST(QuarantineManagerTest, BoundsActivationRootsAndReleasesCleanSlots) {
    auto& manager = QuarantineManager::Process();
    EXPECT_EQ(manager.RetainedActivationCount(), 0U);
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
    ASSERT_EQ(slots.size(), 32U);
    EXPECT_EQ(manager.RetainedActivationCount(), 32U);

    for (const std::uint64_t slot : slots) {
        manager.ReleaseActivation(slot);
    }
    EXPECT_EQ(manager.RetainedActivationCount(), 0U);

    for (std::uint64_t& slot : slots) {
        auto reserved = manager.ReserveActivation("activation slot reuse test");
        ASSERT_TRUE(reserved.has_value());
        slot = reserved.value();
    }
    auto exhausted = manager.ReserveActivation("activation slot exhaustion test");
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);
    for (const std::uint64_t slot : slots) {
        manager.ReleaseActivation(slot);
    }
    EXPECT_EQ(manager.RetainedActivationCount(), 0U);
}

}  // namespace
}  // namespace rodaq::runtime
