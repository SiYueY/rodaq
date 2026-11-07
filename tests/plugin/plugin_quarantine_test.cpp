#include "plugin/plugin_quarantine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace rodaq {
namespace {

TEST(PluginQuarantineTest, CompletesAndReapsSupervisedCalls) {
    auto& manager = plugin::PluginQuarantine::process();
    const std::size_t before = manager.retained_call_count();
    auto outcome = manager.run(
        [] { return Result<void>(); }, std::chrono::steady_clock::now() + std::chrono::seconds(1),
        "completed call");
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.value().status, plugin::SupervisedCallStatus::Completed);
    EXPECT_TRUE(outcome.value().error.ok());
    EXPECT_EQ(manager.retained_call_count(), before);
}

TEST(PluginQuarantineTest, PreservesFailureStatusAndDiagnostics) {
    auto outcome = plugin::PluginQuarantine::process().run(
        [] { return Status(StatusCode::StorageError, "test_storage", 17, "write failed"); },
        std::chrono::steady_clock::now() + std::chrono::seconds(1), "failed call");
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome->status, plugin::SupervisedCallStatus::Failed);
    EXPECT_FALSE(outcome->error.ok());
    EXPECT_EQ(outcome->error.code(), StatusCode::StorageError);
    EXPECT_EQ(outcome->error.domain(), "test_storage");
    EXPECT_EQ(outcome->error.error_code(), 17);
    EXPECT_EQ(outcome->error.message(), "write failed");
}

TEST(PluginQuarantineTest, RetainsThenJoinsTimedOutCall) {
    auto& manager = plugin::PluginQuarantine::process();
    const std::size_t before = manager.retained_call_count();
    auto gate = std::make_shared<std::atomic<bool>>(false);
    auto outcome = manager.run(
        [gate] {
            while (!gate->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Result<void>();
        },
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1), "timed out call");
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.value().status, plugin::SupervisedCallStatus::TimedOut);
    EXPECT_GT(manager.retained_call_count(), before);

    gate->store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (manager.retained_call_count() != before && std::chrono::steady_clock::now() < deadline) {
        manager.reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(manager.retained_call_count(), before);
}

TEST(PluginQuarantineTest, ConcurrentReapersJoinOnceAndReleaseStateOutsideManagerLock) {
    auto& manager = plugin::PluginQuarantine::process();
    manager.reap_completed();
    const auto before = manager.retained_worker_count();
    auto released = std::make_shared<std::atomic<bool>>(false);
    auto destroyed = std::make_shared<std::atomic<int>>(0);
    struct WorkerState {
        std::shared_ptr<std::atomic<int>> destroyed;
        ~WorkerState() {
            // Capture destruction may call DAQ infrastructure after Join.
            static_cast<void>(plugin::PluginQuarantine::process().retained_call_count());
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
        ASSERT_TRUE(manager.retain_worker(std::move(worker), state, done, "reaper race"));
    }
    EXPECT_EQ(manager.retained_worker_count(), before + kWorkers);
    auto reap = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (manager.retained_worker_count() != before &&
               std::chrono::steady_clock::now() < deadline) {
            manager.reap_completed();
            std::this_thread::yield();
        }
    };
    std::thread first(reap);
    std::thread second(reap);
    released->store(true, std::memory_order_release);
    first.join();
    second.join();
    manager.reap_completed();
    manager.reap_completed();
    EXPECT_EQ(manager.retained_worker_count(), before);
    EXPECT_EQ(destroyed->load(), kWorkers);
}

TEST(PluginQuarantineTest, CompletedWorkerMayReenterReapingWithoutJoiningItself) {
    auto& manager = plugin::PluginQuarantine::process();
    const auto before = manager.retained_worker_count();
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto returned = std::make_shared<std::atomic<bool>>(false);
    auto state = std::make_shared<int>(7);
    std::thread worker([&manager, entered, done, returned] {
        while (!entered->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        done->store(true, std::memory_order_release);
        manager.reap_completed();
        returned->store(true, std::memory_order_release);
    });
    ASSERT_TRUE(manager.retain_worker(std::move(worker), state, done, "self-reap test"));
    entered->store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (manager.retained_worker_count() != before &&
           std::chrono::steady_clock::now() < deadline) {
        manager.reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(manager.retained_worker_count(), before);
    EXPECT_TRUE(returned->load(std::memory_order_acquire));
}

TEST(PluginQuarantineTest, CaptureDestructionCompletesBeforeJoinPublication) {
    auto& manager = plugin::PluginQuarantine::process();
    const auto before = manager.retained_call_count();
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
    auto outcome = manager.run(
        std::move(operation), std::chrono::steady_clock::now() + std::chrono::milliseconds(10),
        "blocking capture destruction", joined);
    ASSERT_TRUE(outcome.ok());
    EXPECT_EQ(outcome->status, plugin::SupervisedCallStatus::TimedOut);
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered->load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline) {
        std::this_thread::yield();
    }
    EXPECT_TRUE(entered->load(std::memory_order_acquire));
    EXPECT_FALSE(joined->load(std::memory_order_acquire));
    release->store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!joined->load(std::memory_order_acquire) || manager.retained_call_count() != before) &&
           std::chrono::steady_clock::now() < deadline) {
        manager.reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(joined->load(std::memory_order_acquire));
    EXPECT_EQ(manager.retained_call_count(), before);
}

TEST(PluginQuarantineTest, BoundsActivationRootsAndReleasesCleanSlots) {
    auto& manager = plugin::PluginQuarantine::process();
    EXPECT_EQ(manager.retained_activation_count(), 0U);
    std::vector<std::uint64_t> slots;
    while (true) {
        auto slot = manager.reserve_activation("activation slot test");
        if (!slot.ok()) {
            EXPECT_EQ(slot.status().code(), StatusCode::ResourceExhausted);
            break;
        }
        slots.push_back(slot.value());
        ASSERT_LE(slots.size(), 64U);
    }
    ASSERT_EQ(slots.size(), 32U);
    EXPECT_EQ(manager.retained_activation_count(), 32U);

    for (const std::uint64_t slot : slots) {
        manager.release_activation(slot);
    }
    EXPECT_EQ(manager.retained_activation_count(), 0U);

    for (std::uint64_t& slot : slots) {
        auto reserved = manager.reserve_activation("activation slot reuse test");
        ASSERT_TRUE(reserved.ok());
        slot = reserved.value();
    }
    auto exhausted = manager.reserve_activation("activation slot exhaustion test");
    ASSERT_FALSE(exhausted.ok());
    EXPECT_EQ(exhausted.status().code(), StatusCode::ResourceExhausted);
    for (const std::uint64_t slot : slots) {
        manager.release_activation(slot);
    }
    EXPECT_EQ(manager.retained_activation_count(), 0U);
}

TEST(PluginQuarantineTest, SharedCapacityRejectsWithoutTakingWorkerAndSlotsAreReusable) {
    auto& manager = plugin::PluginQuarantine::process();
    auto release = std::make_shared<std::atomic<bool>>(false);
    std::vector<std::shared_ptr<std::atomic<bool>>> joined;
    joined.reserve(64);
    for (int index = 0; index < 32; ++index) {
        auto signal = std::make_shared<std::atomic<bool>>(false);
        joined.push_back(signal);
        auto result = manager.run(
            [release] {
                while (!release->load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                return Result<void>();
            },
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1), "capacity call",
            signal);
        EXPECT_TRUE(result.ok());
        if (result.ok()) {
            EXPECT_EQ(result.value().status, plugin::SupervisedCallStatus::TimedOut);
        }
    }
    for (int index = 0; index < 32; ++index) {
        auto signal = std::make_shared<std::atomic<bool>>(false);
        auto done = std::make_shared<std::atomic<bool>>(false);
        joined.push_back(signal);
        std::thread worker([release, done] {
            while (!release->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            done->store(true, std::memory_order_release);
        });
        const bool retained =
            manager.retain_worker(std::move(worker), release, done, "capacity worker", signal);
        EXPECT_TRUE(retained);
        if (!retained) {
            release->store(true, std::memory_order_release);
            // A failed retention does not acquire the caller's thread.
            // NOLINTNEXTLINE(bugprone-use-after-move)
            worker.join();
        }
    }
    EXPECT_EQ(manager.retained_call_count(), 32U);
    EXPECT_EQ(manager.retained_worker_count(), 32U);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread rejected([done] { done->store(true, std::memory_order_release); });
    const auto worker_id = rejected.get_id();
    EXPECT_FALSE(manager.retain_worker(std::move(rejected), release, done, "capacity exhausted"));
    // Exhausted capacity must leave the thread with its caller.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_TRUE(rejected.joinable());
    EXPECT_EQ(rejected.get_id(), worker_id);
    if (rejected.joinable()) {
        rejected.join();
    }
    bool invoked = false;
    auto exhausted = manager.run(
        [&invoked] {
            invoked = true;
            return Result<void>();
        },
        std::chrono::steady_clock::now() + std::chrono::seconds(1), "capacity exhausted");
    EXPECT_FALSE(exhausted.ok());
    EXPECT_FALSE(invoked);
    release->store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((manager.retained_call_count() != 0 || manager.retained_worker_count() != 0) &&
           std::chrono::steady_clock::now() < deadline) {
        manager.reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (const auto& signal : joined) {
        EXPECT_TRUE(signal->load(std::memory_order_acquire));
    }
    EXPECT_EQ(manager.retained_call_count(), 0U);
    EXPECT_EQ(manager.retained_worker_count(), 0U);
    release->store(false, std::memory_order_release);
    auto reused_joined = std::make_shared<std::atomic<bool>>(false);
    auto reused = manager.run(
        [release] {
            while (!release->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Result<void>();
        },
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1), "reused call slot",
        reused_joined);
    EXPECT_TRUE(reused.ok());
    if (reused.ok()) {
        EXPECT_EQ(reused.value().status, plugin::SupervisedCallStatus::TimedOut);
    }
    auto worker_joined = std::make_shared<std::atomic<bool>>(false);
    done->store(false, std::memory_order_release);
    std::thread worker([release, done] {
        while (!release->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        done->store(true, std::memory_order_release);
    });
    const bool retained = manager.retain_worker(
        std::move(worker), release, done, "reused worker slot", worker_joined);
    EXPECT_TRUE(retained);
    release->store(true, std::memory_order_release);
    // Failed retention preserves the caller's thread; a moved-from thread is queryable.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    if (worker.joinable()) {
        worker.join();
    }
    const auto reuse_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((manager.retained_call_count() != 0 || manager.retained_worker_count() != 0) &&
           std::chrono::steady_clock::now() < reuse_deadline) {
        manager.reap_completed();
        std::this_thread::yield();
    }
    EXPECT_TRUE(reused_joined->load());
    EXPECT_TRUE(worker_joined->load());
    EXPECT_EQ(manager.retained_call_count(), 0U);
    EXPECT_EQ(manager.retained_worker_count(), 0U);
}

}  // namespace
}  // namespace rodaq
