#include "admission/admission_state.hpp"

#include <rodaq/daq_config.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace rodaq {
namespace {

using Clock = std::chrono::steady_clock;

StreamHandle stream_one() { return StreamHandle::from_raw((std::uint64_t{3} << 32U) | 1U); }

TEST(AdmissionStateTest, GateAndSubmissionAccountingAreIndependent) {
    DAQConfig config;
    AdmissionState state(config);
    state.register_stream(stream_one());
    EXPECT_FALSE(state.gate_open());

    EXPECT_TRUE(state.begin_submission());
    EXPECT_FALSE(state.wait_for_submissions(Clock::now() + std::chrono::milliseconds(1)));
    state.end_submission();
    EXPECT_TRUE(state.wait_for_submissions(Clock::now() + std::chrono::milliseconds(1)));

    state.open_gate();
    EXPECT_TRUE(state.gate_open());
    state.close_gate();
    EXPECT_FALSE(state.gate_open());
}

TEST(AdmissionStateTest, AcceptanceSequenceIsMonotonicPerStream) {
    DAQConfig config;
    AdmissionState state(config);
    state.register_stream(stream_one());

    const auto first = state.allocate(stream_one());
    const auto second = state.allocate(stream_one());
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(0U, first.value());
    EXPECT_EQ(1U, second.value());
}

TEST(AdmissionStateTest, DrainWaitsForBothSubmissionAndAdmissionInflight) {
    DAQConfig config;
    AdmissionState state(config);
    state.register_stream(stream_one());
    state.begin_submission();
    state.record_inflight_acquired();

    EXPECT_FALSE(state.wait_for_drain(Clock::now() + std::chrono::milliseconds(1)));
    state.end_submission();
    EXPECT_FALSE(state.wait_for_drain(Clock::now() + std::chrono::milliseconds(1)));
    state.record_inflight_released();
    EXPECT_TRUE(state.wait_for_drain(Clock::now() + std::chrono::milliseconds(1)));
}

TEST(AdmissionStateTest, EligibilityGenerationAdvancesAndWakesWaiters) {
    DAQConfig config;
    AdmissionState state(config);
    const std::uint64_t initial = state.eligibility_generation();
    state.begin_submission();

    std::thread waiter([&] { state.wait_for_submissions(Clock::now() + std::chrono::seconds(2)); });
    state.notify_eligibility_changed();
    state.end_submission();
    waiter.join();

    EXPECT_GT(state.eligibility_generation(), initial);
    EXPECT_TRUE(state.wait_for_submissions(Clock::now() + std::chrono::milliseconds(1)));
}

}  // namespace
}  // namespace rodaq
