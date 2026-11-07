#include "activation/activation_state.hpp"

#include <rodaq/daq_config.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <type_traits>

namespace rodaq {
namespace {

using Clock = std::chrono::steady_clock;

static_assert(
    std::is_same_v<decltype(std::declval<const ActivationState&>().config()), const DAQConfig&>);

TEST(ActivationStateTest, TracksLifecycleWithoutUsingDAQStateAsInternalAuthority) {
    auto activation = ActivationState::create(DAQConfig{}, 9);
    ASSERT_TRUE(activation.ok());
    EXPECT_EQ(ActivationPhase::Starting, activation.value()->phase());

    EXPECT_TRUE(activation.value()->mark_running());
    EXPECT_EQ(ActivationPhase::Running, activation.value()->phase());
    activation.value()->mark_stopping();
    EXPECT_EQ(ActivationPhase::Stopping, activation.value()->phase());
    activation.value()->mark_failed();
    EXPECT_EQ(ActivationPhase::Failed, activation.value()->phase());
    activation.value()->mark_stopped();
    EXPECT_EQ(ActivationPhase::Failed, activation.value()->phase());
    EXPECT_TRUE(activation.value()->shutdown_logging(Clock::now() + std::chrono::seconds(1)));
}

TEST(ActivationStateTest, ConfiguredInputAndEffectiveLimitsRemainSeparate) {
    DAQConfig config;
    const std::size_t configured_dispatch = config.resources.dispatch_capacity;
    auto activation = ActivationState::create(config, 2);
    ASSERT_TRUE(activation.ok());
    EXPECT_EQ(configured_dispatch, activation.value()->config().resources.dispatch_capacity);

    ActivationLimits effective = activation.value()->limits();
    effective.dispatch_capacity = configured_dispatch + 5U;
    effective.operation_capacity = configured_dispatch + 5U;
    activation.value()->apply_limits(effective);

    EXPECT_EQ(configured_dispatch, activation.value()->config().resources.dispatch_capacity);
    EXPECT_EQ(configured_dispatch + 5U, activation.value()->limits().dispatch_capacity);
    EXPECT_TRUE(activation.value()->shutdown_logging(Clock::now() + std::chrono::seconds(1)));
}

TEST(ActivationStateTest, PrimaryAndSecondaryFailureHistoryAreDistinct) {
    auto activation = ActivationState::create(DAQConfig{}, 3);
    ASSERT_TRUE(activation.ok());
    activation.value()->request_failure(
        Status(StatusCode::StorageError, "storage", 7, "primary failure"));
    activation.value()->request_failure(
        Status(StatusCode::Timeout, "storage", 8, "secondary failure"));
    activation.value()->record_secondary_failure(
        Status(StatusCode::InternalError, "activation", 9, "cleanup failure"));

    const Status primary = activation.value()->primary_failure();
    EXPECT_EQ(StatusCode::StorageError, primary.code());
    EXPECT_EQ("primary failure", primary.message());
    const auto secondary = activation.value()->secondary_failures();
    ASSERT_TRUE(secondary.ok());
    ASSERT_EQ(2U, secondary.value().size());
    EXPECT_EQ("secondary failure", secondary.value()[0].message());
    EXPECT_EQ("cleanup failure", secondary.value()[1].message());
    EXPECT_EQ(0U, activation.value()->secondary_failure_overflow_count());
    EXPECT_TRUE(activation.value()->shutdown_logging(Clock::now() + std::chrono::seconds(1)));
}

}  // namespace
}  // namespace rodaq
