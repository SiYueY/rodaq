#include "time/clock_correlation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <utility>

namespace {

namespace time = rodaq::time;
namespace persistence = rodaq::persistence;
using namespace std::chrono_literals;

class TimeAuthority : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-clock-owner-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        auto boot = time::parse_boot_identity("12345678-1234-4234-8234-123456789abc");
        ASSERT_TRUE(boot.ok());
        boot_ = std::move(boot).value();
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    auto create(persistence::FileFaultHook faults = {}) {
        return time::ClockCorrelation::create(
            root_, boot_, {100, 1000, 1}, {time::BootArtifactState::Absent, {}}, {0, 1},
            std::move(faults));
    }

    std::filesystem::path root_;
    time::BootIdentity boot_;
};

TEST_F(TimeAuthority, ClockJumpsAdvanceDurableEpochsWithoutChangingStandardPlaybackAnchor) {
    auto created = create();
    ASSERT_TRUE(created.ok());
    auto clock = std::move(created).value();
    auto original = clock->snapshot();
    ASSERT_TRUE(original.ok());
    EXPECT_EQ(original.value().epoch_id, 1);
    EXPECT_EQ(original.value().origin, time::ClockEpochOrigin::Boot);
    ASSERT_TRUE(clock->observe({200, 1500, 1}).ok());
    auto forward = clock->snapshot();
    ASSERT_TRUE(forward.ok());
    ASSERT_TRUE(forward.value().jump.has_value());
    EXPECT_EQ(forward.value().epoch_id, 2);
    EXPECT_EQ(forward.value().jump->delta_ns, 400);
    EXPECT_EQ(forward.value().jump->previous_epoch_id, 1);
    ASSERT_TRUE(clock->observe({300, 1400, 1}).ok());
    auto backward = clock->snapshot();
    ASSERT_TRUE(backward.ok());
    EXPECT_EQ(backward.value().epoch_id, 3);
    ASSERT_TRUE(backward.value().jump.has_value());
    EXPECT_EQ(backward.value().jump->delta_ns, -200);
    EXPECT_EQ(original.value().epoch_id, 1);  // Previously captured facts remain immutable.
    EXPECT_EQ(clock->anchor().monotonic_ns, 100);
    EXPECT_EQ(clock->anchor().realtime_ns, 1000);
    EXPECT_EQ(time::project_playback_time(clock->anchor(), boot_.id, 200).value(), 1100);
    EXPECT_EQ(time::project_playback_time(clock->anchor(), boot_.id, 300).value(), 1200);
    EXPECT_FALSE(clock->observe({299, 1400, 1}).ok());
    EXPECT_EQ(clock->snapshot().value().epoch_id, 3);
}

TEST_F(TimeAuthority, SnapshotsDoNotWaitForEpochReservationFilesystemWork) {
    bool armed = false;
    std::promise<void> entered;
    std::promise<void> released;
    auto resume = released.get_future().share();
    auto created = create([&](persistence::FileStep step) {
        if (armed && step == persistence::FileStep::SyncDirectory) {
            entered.set_value();
            resume.wait();
        }
        return 0;
    });
    ASSERT_TRUE(created.ok());
    auto clock = std::move(created).value();
    armed = true;
    auto observing = std::async(std::launch::async, [&] { return clock->observe({200, 1500, 1}); });
    const bool reached = entered.get_future().wait_for(2s) == std::future_status::ready;
    auto snapshot = std::async(std::launch::async, [&] { return clock->snapshot(); });
    const bool ready = snapshot.wait_for(100ms) == std::future_status::ready;
    released.set_value();
    ASSERT_TRUE(reached);
    ASSERT_TRUE(ready);
    auto old = snapshot.get();
    ASSERT_TRUE(old.ok());
    EXPECT_EQ(old.value().epoch_id, 1);
    ASSERT_TRUE(observing.get().ok());
    EXPECT_EQ(clock->snapshot().value().epoch_id, 2);
}

TEST_F(TimeAuthority, UncertainEpochReservationProtectsFutureSnapshotsAndCannotBeRetriedBlindly) {
    bool armed = false;
    auto created = create([&](persistence::FileStep step) {
        return armed && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(created.ok());
    auto clock = std::move(created).value();
    armed = true;
    EXPECT_FALSE(clock->observe({200, 1500, 1}).ok());
    EXPECT_FALSE(clock->snapshot().ok());
    EXPECT_FALSE(clock->observe({300, 1600, 1}).ok());
    EXPECT_EQ(clock->anchor().realtime_ns, 1000);
    clock.reset();
    auto restarted = time::ClockCorrelation::create(
        root_, boot_, {500, 9000, 1}, {time::BootArtifactState::Unknown, {}}, {0, 1});
    ASSERT_TRUE(restarted.ok());
    EXPECT_EQ(restarted.value()->snapshot().value().epoch_id, 3);
    EXPECT_EQ(
        restarted.value()->snapshot().value().origin, time::ClockEpochOrigin::AuthorityRestart);
    EXPECT_EQ(restarted.value()->anchor().realtime_ns, 1000);
}

TEST_F(TimeAuthority, ReceiveCaptureUsesFreshPairedTimeAndRejectsUnobservedClockJumpWithoutIo) {
    unsigned publications = 0;
    auto created = create([&](persistence::FileStep step) {
        if (step == persistence::FileStep::Publish) {
            ++publications;
        }
        return 0;
    });
    ASSERT_TRUE(created.ok());
    auto& clock = *created.value();
    const auto before = publications;
    auto received = clock.capture_receive({150, 1050, 1});
    ASSERT_TRUE(received.ok());
    EXPECT_EQ(received.value().sample.monotonic_ns, 150);
    EXPECT_EQ(received.value().sample.realtime_ns, 1050);
    EXPECT_EQ(received.value().epoch_id, 1);
    EXPECT_EQ(clock.snapshot().value().sample.monotonic_ns, 100);
    EXPECT_EQ(
        clock.capture_receive({200, 1500, 1}).status().code(), rodaq::StatusCode::Unavailable);
    EXPECT_EQ(publications, before);
    EXPECT_EQ(clock.snapshot().value().epoch_id, 1);
    ASSERT_TRUE(clock.observe({200, 1500, 1}).ok());
    auto after = clock.capture_receive({250, 1550, 1});
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().epoch_id, 2);
    EXPECT_EQ(after.value().sample.monotonic_ns, 250);
    ASSERT_TRUE(after.value().jump);
    EXPECT_EQ(after.value().jump->detected_monotonic_ns, 200);
    EXPECT_EQ(received.value().epoch_id, 1);
    EXPECT_EQ(received.value().sample.realtime_ns, 1050);
    EXPECT_EQ(
        clock.capture_receive({150, 1050, 1}).status().code(), rodaq::StatusCode::Unavailable);
}

TEST_F(TimeAuthority, ReceiveSampleMayPrecedeLatestSameEpochObservationWithoutBeingRelabeled) {
    auto created = create();
    ASSERT_TRUE(created.ok());
    auto& clock = *created.value();
    ASSERT_TRUE(clock.observe({200, 1100, 1}).ok());
    auto concurrent_receive = clock.capture_receive({150, 1050, 1});
    ASSERT_TRUE(concurrent_receive.ok());
    EXPECT_EQ(concurrent_receive.value().epoch_id, 1);
    EXPECT_EQ(concurrent_receive.value().sample.monotonic_ns, 150);
    EXPECT_EQ(clock.capture_receive({99, 999, 1}).status().code(), rodaq::StatusCode::Unavailable);
}

TEST_F(
    TimeAuthority, ReceiveCaptureDoesNotWaitForBlockedEpochFsyncOrAssignNewCorrelationToOldEpoch) {
    bool armed = false;
    std::promise<void> entered;
    std::promise<void> release;
    auto permission = release.get_future().share();
    auto created = create([&](persistence::FileStep step) {
        if (armed && step == persistence::FileStep::SyncDirectory) {
            entered.set_value();
            permission.wait();
        }
        return 0;
    });
    ASSERT_TRUE(created.ok());
    auto& clock = *created.value();
    armed = true;
    auto observation = std::async(std::launch::async, [&] {
        return clock.observe({200, 1500, 1});
    });
    const auto writing = entered.get_future().wait_for(2s);
    auto capture = std::async(std::launch::async, [&] {
        return clock.capture_receive({250, 1550, 1});
    });
    const auto immediate = capture.wait_for(100ms);
    release.set_value();
    EXPECT_EQ(writing, std::future_status::ready);
    EXPECT_EQ(immediate, std::future_status::ready);
    EXPECT_EQ(capture.get().status().code(), rodaq::StatusCode::Unavailable);
    ASSERT_TRUE(observation.get().ok());
    auto after = clock.capture_receive({250, 1550, 1});
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value().epoch_id, 2);
}

}  // namespace
