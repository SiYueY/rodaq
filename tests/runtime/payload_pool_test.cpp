#include <rodaq/runtime/payload.hpp>
#include <rodaq/runtime/payload_pool.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace rodaq {
namespace {

TEST(PayloadViewTest, EmptyAndNonEmptyViews) {
    const PayloadView empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.size(), 0U);

    const std::array<std::uint8_t, 3> bytes{1, 2, 3};
    const PayloadView view(bytes.data(), bytes.size());
    EXPECT_FALSE(view.empty());
    EXPECT_EQ(view.data(), bytes.data());
    EXPECT_EQ(view.size(), 3U);
    EXPECT_TRUE(view.valid());
}

TEST(PayloadViewTest, PreservesInvalidNonEmptyView) {
    const PayloadView invalid(nullptr, 4);
    EXPECT_FALSE(invalid.empty());
    EXPECT_FALSE(invalid.valid());
    EXPECT_EQ(invalid.data(), nullptr);
    EXPECT_EQ(invalid.size(), 4U);
}

TEST(PayloadTest, MoveTransfersOwnershipAndLeavesSourceEmpty) {
    auto pool = PayloadPool::create(16, 1);
    ASSERT_TRUE(pool.has_value());
    const std::array<std::uint8_t, 4> source{1, 2, 3, 4};

    auto acquired = pool.value().acquire(source.size(), PayloadView(source.data(), source.size()));
    ASSERT_TRUE(acquired.has_value());
    const std::uint8_t* original_data = acquired.value().view().data();

    Payload moved(std::move(acquired.value()));
    EXPECT_TRUE(acquired.value().empty());
    EXPECT_EQ(moved.view().data(), original_data);
    EXPECT_EQ(pool.value().used_bytes(), 16U);

    Payload target;
    target = std::move(moved);
    // Verify the specified empty moved-from state.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    EXPECT_TRUE(moved.empty());
    EXPECT_EQ(target.size(), source.size());
    EXPECT_EQ(pool.value().used_bytes(), 16U);
}

TEST(PayloadTest, MoveAssignmentReleasesPreviousPayload) {
    auto pool = PayloadPool::create(8, 1);
    ASSERT_TRUE(pool.has_value());
    const std::array<std::uint8_t, 1> first_source{1};
    const std::array<std::uint8_t, 1> second_source{2};

    auto first = pool.value().acquire(
        first_source.size(), PayloadView(first_source.data(), first_source.size()));
    ASSERT_TRUE(first.has_value());
    auto second = pool.value().acquire(
        second_source.size(), PayloadView(second_source.data(), second_source.size()));
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code(), ErrorCode::ResourceExhausted);

    Payload target(std::move(first.value()));
    auto second_attempt = pool.value().acquire(
        second_source.size(), PayloadView(second_source.data(), second_source.size()));
    ASSERT_FALSE(second_attempt.has_value());

    target = Payload();
    EXPECT_EQ(pool.value().used_bytes(), 0U);
    second_attempt = pool.value().acquire(
        second_source.size(), PayloadView(second_source.data(), second_source.size()));
    ASSERT_TRUE(second_attempt.has_value());
    EXPECT_EQ(second_attempt.value().view().data()[0], 2U);
}

namespace {

struct ReleaseProbe {
    int calls = 0;
};

void release_probe(void* owner_context) noexcept {
    auto* probe = static_cast<ReleaseProbe*>(owner_context);
    ++probe->calls;
}

}  // namespace

TEST(PayloadTest, AdoptTransfersExternalLifetimeAnchorExactlyOnce) {
    const std::array<std::uint8_t, 3> source{4, 5, 6};
    ReleaseProbe probe;
    {
        auto adopted =
            Payload::adopt(PayloadView(source.data(), source.size()), &probe, &release_probe);
        ASSERT_TRUE(adopted.has_value());
        EXPECT_EQ(adopted.value().view().data(), source.data());

        Payload moved(std::move(adopted.value()));
        EXPECT_TRUE(adopted.value().empty());
        EXPECT_EQ(probe.calls, 0);
        EXPECT_EQ(moved.size(), source.size());
    }
    EXPECT_EQ(probe.calls, 1);
}

TEST(PayloadTest, RetainKeepsExternalAnchorExactlyOnce) {
    const std::array<std::uint8_t, 2> source{7, 8};
    ReleaseProbe probe;
    {
        auto retained =
            Payload::retain(PayloadView(source.data(), source.size()), &probe, &release_probe);
        ASSERT_TRUE(retained.has_value());
        EXPECT_EQ(retained.value().view().data(), source.data());
    }
    EXPECT_EQ(probe.calls, 1);
}

TEST(PayloadTest, MoveRetainsPluginLifetimeGuardWithExternalAnchor) {
    const std::array<std::uint8_t, 2> source{7, 8};
    ReleaseProbe release;
    int guard_destroys = 0;
    {
        auto guard = std::shared_ptr<void>(new int(1), [&guard_destroys](void* value) {
            delete static_cast<int*>(value);
            ++guard_destroys;
        });
        auto retained =
            Payload::retain(PayloadView(source.data(), source.size()), &release, &release_probe);
        ASSERT_TRUE(retained.has_value());
        retained.value().attach_lifetime_guard(guard);

        Payload moved(std::move(retained.value()));
        EXPECT_TRUE(retained.value().empty());
        EXPECT_EQ(0, guard_destroys);
        EXPECT_EQ(0, release.calls);
        EXPECT_EQ(moved.size(), source.size());
    }
    EXPECT_EQ(1, release.calls);
    EXPECT_EQ(1, guard_destroys);
}

TEST(PayloadTest, ExternalModesRejectMissingReleaseCapability) {
    const std::array<std::uint8_t, 1> source{1};
    const auto adopted =
        Payload::adopt(PayloadView(source.data(), source.size()), nullptr, nullptr);
    ASSERT_FALSE(adopted.has_value());
    EXPECT_EQ(adopted.error().code(), ErrorCode::InvalidArgument);

    const auto retained =
        Payload::retain(PayloadView(source.data(), source.size()), nullptr, nullptr);
    ASSERT_FALSE(retained.has_value());
    EXPECT_EQ(retained.error().code(), ErrorCode::InvalidArgument);
}

TEST(PayloadTest, AdoptAndRetainValidateBeforeAcquiringResponsibility) {
    ReleaseProbe probe;
    const PayloadView invalid(nullptr, 3);

    const auto adopted = Payload::adopt(invalid, &probe, &release_probe);
    ASSERT_FALSE(adopted.has_value());
    EXPECT_EQ(adopted.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(probe.calls, 0);

    const auto retained = Payload::retain(invalid, &probe, &release_probe);
    ASSERT_FALSE(retained.has_value());
    EXPECT_EQ(retained.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(probe.calls, 0);
}

TEST(PayloadTest, EmptyExternalPayloadStillOwnsItsAnchor) {
    ReleaseProbe adopt_probe;
    {
        auto adopted = Payload::adopt(PayloadView(), &adopt_probe, &release_probe);
        ASSERT_TRUE(adopted.has_value());
        EXPECT_TRUE(adopted.value().empty());
        EXPECT_EQ(adopt_probe.calls, 0);
    }
    EXPECT_EQ(adopt_probe.calls, 1);

    ReleaseProbe retain_probe;
    {
        auto retained = Payload::retain(PayloadView(), &retain_probe, &release_probe);
        ASSERT_TRUE(retained.has_value());
        EXPECT_TRUE(retained.value().empty());
        EXPECT_EQ(retain_probe.calls, 0);
    }
    EXPECT_EQ(retain_probe.calls, 1);
}

TEST(PayloadPoolTest, OwnsExactLogicalBytes) {
    auto pool = PayloadPool::create(32, 2);
    ASSERT_TRUE(pool.has_value());
    const std::array<std::uint8_t, 5> source{9, 8, 7, 6, 5};

    auto payload = pool.value().acquire(source.size(), PayloadView(source.data(), source.size()));
    ASSERT_TRUE(payload.has_value());
    ASSERT_EQ(payload.value().size(), source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        EXPECT_EQ(payload.value().view().data()[index], source[index]);
    }
}

TEST(PayloadPoolTest, ZeroLengthPayloadDoesNotConsumePoolStorage) {
    auto pool = PayloadPool::create(8, 1);
    ASSERT_TRUE(pool.has_value());

    const auto empty = pool.value().try_reserve(0);
    ASSERT_TRUE(empty.has_value());
    EXPECT_FALSE(empty.value().active());
    EXPECT_EQ(pool.value().used_bytes(), 0U);
    EXPECT_EQ(pool.value().available_bytes(), 8U);
}

TEST(PayloadPoolTest, ReusesReleasedStorage) {
    auto pool = PayloadPool::create(8, 1);
    ASSERT_TRUE(pool.has_value());
    const std::array<std::uint8_t, 8> source{};

    auto first = pool.value().acquire(source.size(), PayloadView(source.data(), source.size()));
    ASSERT_TRUE(first.has_value());
    const std::uint8_t* first_data = first.value().view().data();

    const auto exhausted = pool.value().try_reserve(1);
    ASSERT_FALSE(exhausted.has_value());
    EXPECT_EQ(exhausted.error().code(), ErrorCode::ResourceExhausted);

    first.value() = Payload();
    auto second = pool.value().acquire(source.size(), PayloadView(source.data(), source.size()));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value().view().data(), first_data);
}

TEST(PayloadPoolTest, TracksUsedAvailableAndHighWaterBytes) {
    auto pool = PayloadPool::create(16, 3);
    ASSERT_TRUE(pool.has_value());

    auto first = pool.value().try_reserve(3);
    auto second = pool.value().try_reserve(5);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(pool.value().capacity_bytes(), 48U);
    EXPECT_EQ(pool.value().used_bytes(), 32U);
    EXPECT_EQ(pool.value().available_bytes(), 16U);
    EXPECT_EQ(pool.value().high_water_bytes(), 32U);

    first.value() = PayloadPoolReservation();
    EXPECT_EQ(pool.value().used_bytes(), 16U);
    EXPECT_EQ(pool.value().available_bytes(), 32U);
    EXPECT_EQ(pool.value().high_water_bytes(), 32U);
}

TEST(PayloadPoolTest, RejectsInvalidConfigurationAndOversizedRequests) {
    const auto zero_size = PayloadPool::create(0, 1);
    ASSERT_FALSE(zero_size.has_value());
    EXPECT_EQ(zero_size.error().code(), ErrorCode::InvalidArgument);

    const auto zero_count = PayloadPool::create(1, 0);
    ASSERT_FALSE(zero_count.has_value());
    EXPECT_EQ(zero_count.error().code(), ErrorCode::InvalidArgument);

    const auto overflow = PayloadPool::create(std::numeric_limits<std::size_t>::max(), 2);
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), ErrorCode::InvalidArgument);

    auto pool = PayloadPool::create(4, 1);
    ASSERT_TRUE(pool.has_value());
    const auto oversized = pool.value().try_reserve(5);
    ASSERT_FALSE(oversized.has_value());
    EXPECT_EQ(oversized.error().code(), ErrorCode::ResourceExhausted);
}

TEST(PayloadPoolTest, RejectsMismatchedSourceSize) {
    auto pool = PayloadPool::create(4, 1);
    ASSERT_TRUE(pool.has_value());
    const std::array<std::uint8_t, 2> source{1, 2};

    const auto mismatch = pool.value().acquire(3, PayloadView(source.data(), source.size()));
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_EQ(mismatch.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(pool.value().used_bytes(), 0U);
}

TEST(PayloadPoolTest, CopyRejectsInvalidSourceWithoutConsumingStorage) {
    auto pool = PayloadPool::create(4, 1);
    ASSERT_TRUE(pool.has_value());

    const auto invalid = pool.value().acquire(4, PayloadView(nullptr, 4));
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(pool.value().used_bytes(), 0U);
}

TEST(PayloadPoolTest, SelectsSmallestClassAndKeepsClassBudgetsIsolated) {
    PayloadPoolConfig config;
    config.classes = {
        PayloadClassConfig{8, 1},
        PayloadClassConfig{64, 1},
    };
    auto pool = PayloadPool::create(config);
    ASSERT_TRUE(pool.has_value());

    auto small = pool.value().try_reserve(4);
    ASSERT_TRUE(small.has_value());
    const auto small_stats = pool.value().stats();
    ASSERT_EQ(2U, small_stats.classes.size());
    EXPECT_EQ(1U, small_stats.classes[0].used_blocks);
    EXPECT_EQ(0U, small_stats.classes[0].available_blocks);
    EXPECT_EQ(0U, small_stats.classes[1].used_blocks);

    const auto small_exhausted = pool.value().try_reserve(4);
    ASSERT_FALSE(small_exhausted.has_value());
    EXPECT_EQ(ErrorCode::ResourceExhausted, small_exhausted.error().code());
    EXPECT_EQ(1U, pool.value().stats().classes[0].exhaustion_count);

    auto large = pool.value().try_reserve(9);
    ASSERT_TRUE(large.has_value());
    const auto large_stats = pool.value().stats();
    EXPECT_EQ(1U, large_stats.classes[0].used_blocks);
    EXPECT_EQ(1U, large_stats.classes[1].used_blocks);
    EXPECT_EQ(8U, large_stats.classes[0].current_physical_bytes);
    EXPECT_EQ(64U, large_stats.classes[1].current_physical_bytes);
    EXPECT_EQ(8U, large_stats.classes[0].high_water_physical_bytes);
    EXPECT_EQ(64U, large_stats.classes[1].high_water_physical_bytes);

    small.value() = PayloadPoolReservation();
    large.value() = PayloadPoolReservation();
    EXPECT_EQ(0U, pool.value().stats().classes[0].used_blocks);
    EXPECT_EQ(0U, pool.value().stats().classes[1].used_blocks);
}

static_assert(std::is_move_constructible<Payload>::value);
static_assert(!std::is_copy_constructible<Payload>::value);

}  // namespace
}  // namespace rodaq
