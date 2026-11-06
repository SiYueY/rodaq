#include "binding/binding_state.hpp"

#include <rodaq/daq_config.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace rodaq {
namespace {

StreamHandle stream_one() { return StreamHandle::from_raw((std::uint64_t{7} << 32U) | 1U); }

TEST(BindingTransitionTest, EnforcesLifecycleOrderAndCurrentPublication) {
    DAQConfig config;
    BindingState state(config, 7);
    state.register_stream(stream_one());
    auto binding = state.create_binding(
        0, stream_one(), StreamDescriptorHandle::from_raw(11), BindingLifecycle::Prepared);
    ASSERT_TRUE(binding.ok());
    EXPECT_EQ(BindingLifecycle::Prepared, state.lifecycle(binding.value()).value());
    EXPECT_FALSE(state.current_binding(stream_one()).valid());

    ASSERT_TRUE(
        state.transition(binding.value(), BindingLifecycle::Prepared, BindingLifecycle::Active)
            .ok());
    state.set_current_binding(stream_one(), binding.value());
    EXPECT_EQ(binding.value(), state.current_binding(stream_one()));
    ASSERT_TRUE(
        state.transition(binding.value(), BindingLifecycle::Active, BindingLifecycle::Draining)
            .ok());
    EXPECT_FALSE(
        state.transition(binding.value(), BindingLifecycle::Active, BindingLifecycle::Draining)
            .ok());
    ASSERT_TRUE(
        state.transition(binding.value(), BindingLifecycle::Draining, BindingLifecycle::Retired)
            .ok());
    EXPECT_EQ(BindingLifecycle::Retired, state.lifecycle(binding.value()).value());
}

TEST(BindingTransitionTest, RetiresOnlyAfterInflightAdmissionDrains) {
    DAQConfig config;
    BindingState state(config, 1);
    state.register_stream(stream_one());
    auto binding = state.create_binding(
        0, stream_one(), StreamDescriptorHandle::from_raw(12), BindingLifecycle::Active);
    ASSERT_TRUE(binding.ok());
    BindingEntry* entry = state.bindings().find(binding.value());
    ASSERT_NE(nullptr, entry);
    entry->admission_inflight->store(1);
    ASSERT_TRUE(
        state.transition(binding.value(), BindingLifecycle::Active, BindingLifecycle::Draining)
            .ok());
    EXPECT_FALSE(state.retire_if_drained(binding.value()));
    entry->admission_inflight->store(0);
    EXPECT_TRUE(state.retire_if_drained(binding.value()));
    EXPECT_EQ(BindingLifecycle::Retired, state.lifecycle(binding.value()).value());
}

TEST(BindingTransitionTest, ConcurrentCompetingTransitionsHaveOneWinner) {
    DAQConfig config;
    BindingState state(config, 1);
    state.register_stream(stream_one());
    auto binding = state.create_binding(
        0, stream_one(), StreamDescriptorHandle::from_raw(13), BindingLifecycle::Prepared);
    ASSERT_TRUE(binding.ok());

    std::atomic<int> successes{0};
    std::thread first([&] {
        if (state.transition(binding.value(), BindingLifecycle::Prepared, BindingLifecycle::Active)
                .ok()) {
            successes.fetch_add(1);
        }
    });
    std::thread second([&] {
        if (state.transition(binding.value(), BindingLifecycle::Prepared, BindingLifecycle::Retired)
                .ok()) {
            successes.fetch_add(1);
        }
    });
    first.join();
    second.join();

    EXPECT_EQ(1, successes.load());
    const auto lifecycle = state.lifecycle(binding.value());
    ASSERT_TRUE(lifecycle.ok());
    EXPECT_TRUE(
        lifecycle.value() == BindingLifecycle::Active ||
        lifecycle.value() == BindingLifecycle::Retired);
}

}  // namespace
}  // namespace rodaq
