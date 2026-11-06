#include "storage/storage_pipeline.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace rodaq {
namespace {

using Clock = std::chrono::steady_clock;

TEST(StoragePipelineTest, EligibilityChangeInterruptsPreGateWait) {
    PayloadPoolConfig pool_config;
    pool_config.classes.push_back(PayloadClassConfig{64, 2});
    auto payload_pool = PayloadPool::create(pool_config);
    ASSERT_TRUE(payload_pool.ok());

    auto* storage = reinterpret_cast<plugin::StoragePluginInstance*>(1);
    StoragePipeline pipeline(
        1, 1, 64, 64, storage, std::chrono::milliseconds(100), 1, [](Status) {},
        [](std::size_t) noexcept {}, [](rodaq_storage_persistence_state) noexcept {},
        [](StorageLatencyKind, std::int64_t) noexcept {},
        [](const AcceptedRecord&, rodaq_storage_operation,
           std::array<rodaq_timestamp_value, TimestampState::kMaxSlots>&) {
            return Result<rodaq_storage_submission>(
                rodaq::Status(StatusCode::InvalidState, "unused"));
        });
    pipeline.open_admission();

    std::atomic<std::uint64_t> eligibility_generation{0};
    auto reserved = pipeline.acquire_pre_gate(
        16, AdmissionMode::Reject, &payload_pool.value(), true,
        Clock::now() + std::chrono::seconds(1), eligibility_generation);
    ASSERT_TRUE(reserved.ok());
    ASSERT_EQ(PreGateAcquireOutcome::Acquired, reserved.value().outcome);

    std::atomic<bool> waiter_started{false};
    PreGateAcquireResult waiter_result;
    const auto wait_started = Clock::now();
    std::thread waiter([&] {
        waiter_started.store(true, std::memory_order_release);
        auto result = pipeline.acquire_pre_gate(
            16, AdmissionMode::Wait, &payload_pool.value(), true,
            Clock::now() + std::chrono::seconds(5), eligibility_generation);
        ASSERT_TRUE(result.ok());
        waiter_result = std::move(result.value());
    });
    while (!waiter_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    eligibility_generation.fetch_add(1, std::memory_order_release);
    pipeline.notify_admission_waiters();
    waiter.join();

    EXPECT_EQ(PreGateAcquireOutcome::EligibilityChanged, waiter_result.outcome);
    EXPECT_LT(Clock::now() - wait_started, std::chrono::seconds(1));
}

}  // namespace
}  // namespace rodaq
