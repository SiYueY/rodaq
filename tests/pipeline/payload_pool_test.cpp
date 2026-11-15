#include "pipeline/payload_pool.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <future>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace pipeline = rodaq::pipeline;
using rodaq::StatusCode;

static_assert(!std::is_copy_constructible_v<pipeline::Payload>);
static_assert(std::is_nothrow_move_constructible_v<pipeline::Payload>);

TEST(PayloadPool, ActualPageChargeAndAllResourceFailuresRollbackBeforeAdmission) {
    auto created = pipeline::PayloadPool::create({1024, 256, 1024, 4});
    ASSERT_TRUE(created.ok());
    auto& pool = created.value();
    auto first = pool.reserve(257);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().physical_charge(), 512);
    auto second = pool.reserve(1);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(pool.snapshot().used_physical_bytes, 768);
    EXPECT_EQ(pool.reserve(257).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(pool.reserve(1025).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(pool.snapshot().used_physical_bytes, 768);
    EXPECT_EQ(pool.snapshot().live_buffers, 2);
    second.value().reset();
    auto fitting = pool.reserve(512);
    ASSERT_TRUE(fitting.ok());
    EXPECT_EQ(pool.snapshot().used_physical_bytes, 1024);
    fitting.value().reset();
    first.value().reset();
    EXPECT_EQ(pool.snapshot().used_physical_bytes, 0);
    EXPECT_EQ(pool.snapshot().live_buffers, 0);
    EXPECT_EQ(pool.snapshot().high_water_physical_bytes, 1024);
}

TEST(PayloadPool, CopyOwnsBytesBeforeBorrowedSourceChangesAndInvalidCopyDoesNotPublishAnything) {
    auto created = pipeline::PayloadPool::create({1024, 256, 1024, 4});
    ASSERT_TRUE(created.ok());
    auto payload = created.value().reserve(4);
    ASSERT_TRUE(payload.ok());
    std::array<unsigned char, 4> source{1, 2, 3, 4};
    EXPECT_EQ(payload.value().copy_from(nullptr, 4).status().code(), StatusCode::InvalidArgument);
    EXPECT_EQ(
        payload.value().copy_from(source.data(), 3).status().code(), StatusCode::InvalidArgument);
    ASSERT_TRUE(payload.value().copy_from(source.data(), source.size()).ok());
    source.fill(9);
    const std::array<unsigned char, 4> expected{1, 2, 3, 4};
    EXPECT_EQ(std::memcmp(payload.value().data(), expected.data(), expected.size()), 0);
    EXPECT_EQ(payload.value().size(), 4);
    payload.value().reset();
    EXPECT_EQ(
        payload.value().copy_from(expected.data(), expected.size()).status().code(),
        StatusCode::InvalidState);
    EXPECT_EQ(created.value().snapshot().live_buffers, 0);
}

TEST(PayloadPool, ZeroLengthPayloadStillHasBoundedOwnerCardinality) {
    auto created = pipeline::PayloadPool::create({1024, 256, 1024, 2});
    ASSERT_TRUE(created.ok());
    auto& pool = created.value();
    auto first = pool.reserve(0);
    auto second = pool.reserve(0);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(static_cast<bool>(first.value()));
    EXPECT_EQ(first.value().data(), nullptr);
    EXPECT_EQ(first.value().physical_charge(), 0);
    EXPECT_TRUE(first.value().copy_from(nullptr, 0).ok());
    EXPECT_EQ(pool.reserve(0).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(pool.snapshot().used_physical_bytes, 0);
    EXPECT_EQ(pool.snapshot().live_buffers, 2);
    first.value().reset();
    EXPECT_TRUE(pool.reserve(256).ok());
    EXPECT_EQ(pool.snapshot().live_buffers, 1);
}

TEST(PayloadPool, FragmentedFreeBytesCauseExplicitPreAdmissionFailureWithoutArenaGrowth) {
    auto created = pipeline::PayloadPool::create({1024, 256, 1024, 8});
    ASSERT_TRUE(created.ok());
    auto& pool = created.value();
    auto first = pool.reserve(256);
    auto second = pool.reserve(256);
    auto third = pool.reserve(256);
    auto fourth = pool.reserve(256);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_TRUE(third.ok());
    ASSERT_TRUE(fourth.ok());
    first.value().reset();
    third.value().reset();
    EXPECT_EQ(pool.snapshot().used_physical_bytes, 512);
    EXPECT_EQ(pool.reserve(512).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(pool.snapshot().capacity_bytes, 1024);
    second.value().reset();
    EXPECT_TRUE(pool.reserve(512).ok());
}

TEST(PayloadPool, MoveOwnershipAndPoolFacadeDestructionKeepActualAccessBackingAlive) {
    std::optional<pipeline::PayloadPool> pool;
    auto created = pipeline::PayloadPool::create({1024, 256, 1024, 4});
    ASSERT_TRUE(created.ok());
    pool.emplace(std::move(created).value());
    auto reserved = pool->reserve(4);
    ASSERT_TRUE(reserved.ok());
    auto payload = std::move(reserved).value();
    const std::array<unsigned char, 4> source{1, 2, 3, 4};
    ASSERT_TRUE(payload.copy_from(source.data(), source.size()).ok());
    auto spare = pool->reserve(1);
    ASSERT_TRUE(spare.ok());
    auto moved = std::move(spare).value();
    moved = std::move(payload);
    EXPECT_FALSE(static_cast<bool>(payload));
    EXPECT_EQ(payload.size(), 0);
    EXPECT_EQ(pool->snapshot().live_buffers, 1);
    std::promise<void> release;
    auto permission = release.get_future();
    auto actual_storage_access = std::async(
        std::launch::async,
        [owned = std::move(moved), permission = std::move(permission), source]() mutable {
            permission.wait();
            const bool intact = std::memcmp(owned.data(), source.data(), source.size()) == 0;
            owned.reset();
            return intact;
        });
    pool.reset();
    release.set_value();
    EXPECT_TRUE(actual_storage_access.get());
}

TEST(PayloadPool, ConcurrentReservationCopyReleaseAndSnapshotsRespectExactCapacity) {
    auto created = pipeline::PayloadPool::create({4096, 256, 512, 16});
    ASSERT_TRUE(created.ok());
    auto& pool = created.value();
    std::atomic<bool> finished{false};
    auto observer = std::async(std::launch::async, [&] {
        bool valid = true;
        while (!finished) {
            const auto snapshot = pool.snapshot();
            valid = valid && snapshot.used_physical_bytes <= 4096 && snapshot.live_buffers <= 8 &&
                    snapshot.high_water_physical_bytes <= 4096;
        }
        return valid;
    });
    std::vector<std::future<bool>> workers;
    for (unsigned thread = 0; thread < 8; ++thread) {
        workers.push_back(std::async(std::launch::async, [&, thread] {
            std::array<unsigned char, 257> source{};
            source.fill(static_cast<unsigned char>(thread));
            for (unsigned iteration = 0; iteration < 128; ++iteration) {
                auto payload = pool.reserve(source.size());
                if (!payload.ok() ||
                    !payload.value().copy_from(source.data(), source.size()).ok() ||
                    std::memcmp(payload.value().data(), source.data(), source.size()) != 0) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& worker : workers) {
        EXPECT_TRUE(worker.get());
    }
    finished = true;
    EXPECT_TRUE(observer.get());
    EXPECT_EQ(pool.snapshot().used_physical_bytes, 0);
    EXPECT_EQ(pool.snapshot().live_buffers, 0);
}

TEST(PayloadPool, InvalidGeometryAndMovedFacadeCannotCreateUnaccountedBacking) {
    EXPECT_FALSE(pipeline::PayloadPool::create({1024, 0, 512, 4}).ok());
    EXPECT_FALSE(pipeline::PayloadPool::create({1024, 257, 512, 4}).ok());
    EXPECT_FALSE(pipeline::PayloadPool::create({1023, 256, 512, 4}).ok());
    EXPECT_FALSE(pipeline::PayloadPool::create({1024, 256, 1025, 4}).ok());
    EXPECT_FALSE(pipeline::PayloadPool::create({1024, 256, 512, 0}).ok());
    auto created = pipeline::PayloadPool::create({1024, 256, 512, 4});
    ASSERT_TRUE(created.ok());
    auto moved = std::move(created).value();
    EXPECT_EQ(created.value().reserve(1).status().code(), StatusCode::InvalidState);
    EXPECT_EQ(created.value().snapshot().capacity_bytes, 0);
    EXPECT_TRUE(moved.reserve(512).ok());
}

}  // namespace
