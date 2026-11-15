#include "pipeline/admission_gate.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace pipeline = rodaq::pipeline;
using rodaq::StatusCode;
using namespace std::chrono_literals;

pipeline::InstanceId instance(unsigned char suffix = 1) {
    pipeline::InstanceId id;
    id.value[15] = suffix;
    return id;
}

static_assert(!std::is_copy_constructible_v<pipeline::AdmissionCommit>);
static_assert(std::is_nothrow_move_constructible_v<pipeline::AdmissionCommit>);

TEST(AdmissionGate, ScopeAndDurableGenerationMustMatchBeforeAnyCommit) {
    pipeline::AdmissionGate gate(instance(), 10);
    EXPECT_EQ(gate.enter_commit(instance(), 10).status().code(), StatusCode::InvalidState);
    EXPECT_EQ(gate.open_admission(instance(2), 10), pipeline::GateOpenResult::Obsolete);
    EXPECT_EQ(gate.open_admission(instance(), 9), pipeline::GateOpenResult::Obsolete);
    EXPECT_EQ(gate.open_admission(instance(), 11), pipeline::GateOpenResult::Obsolete);
    EXPECT_FALSE(gate.snapshot().open);
    EXPECT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Opened);
    {
        auto commit = gate.enter_commit(instance(), 10);
        ASSERT_TRUE(commit.ok());
        EXPECT_TRUE(commit.value().mark_committed());
        EXPECT_FALSE(commit.value().mark_committed());
    }
    EXPECT_EQ(gate.snapshot().accepted_records, 1);
    EXPECT_EQ(gate.enter_commit(instance(2), 10).status().code(), StatusCode::Conflict);
    EXPECT_EQ(gate.enter_commit(instance(), 9).status().code(), StatusCode::Conflict);
}

TEST(AdmissionGate, InMemoryFenceImmediatelyRejectsLateOpenAndSurvivesPersistenceFailure) {
    pipeline::AdmissionGate gate(instance(), 10);
    ASSERT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Opened);
    EXPECT_EQ(gate.establish_control_fence(instance(2)).status().code(), StatusCode::Conflict);
    EXPECT_TRUE(gate.snapshot().open);
    EXPECT_EQ(gate.close_and_capture(instance(2)).status().code(), StatusCode::Conflict);
    EXPECT_TRUE(gate.snapshot().open);
    ASSERT_TRUE(gate.establish_control_fence(instance()).ok());
    EXPECT_FALSE(gate.snapshot().open);
    EXPECT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Obsolete);
    EXPECT_EQ(gate.enter_commit(instance(), 10).status().code(), StatusCode::Conflict);
    // No durable advancement occurred, e.g. fsync failed. The old authority
    // remaining on disk cannot silently clear the safety fence.
    EXPECT_EQ(gate.snapshot().authority_generation, 10);
    EXPECT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Obsolete);
    ASSERT_TRUE(gate.advance_authority(instance(), 11).ok());
    EXPECT_EQ(gate.snapshot().authority_generation, 11);
    EXPECT_EQ(gate.open_admission(instance(), 11), pipeline::GateOpenResult::Obsolete);
    EXPECT_EQ(gate.advance_authority(instance(), 10).status().code(), StatusCode::Conflict);
    pipeline::AdmissionGate replacement(instance(2), 12);
    EXPECT_EQ(replacement.open_admission(instance(), 10), pipeline::GateOpenResult::Obsolete);
    EXPECT_EQ(replacement.open_admission(instance(2), 12), pipeline::GateOpenResult::Opened);
}

TEST(AdmissionGate, FenceDoesNotWaitForExistingCoreCommitButCutWaitsForItsActualSettlement) {
    pipeline::AdmissionGate gate(instance(), 10);
    ASSERT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Opened);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    auto producer = std::async(std::launch::async, [&] {
        auto commit = gate.enter_commit(instance(), 10);
        if (!commit.ok()) {
            return false;
        }
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        return commit.value().mark_committed();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 5s, [&] { return entered; }));
    }
    // These calls must finish even while the existing guard still owns Gate.
    auto fence =
        std::async(std::launch::async, [&] { return gate.establish_control_fence(instance()); });
    const auto fenced = fence.wait_for(1s);
    if (fenced == std::future_status::ready) {
        EXPECT_TRUE(fence.get().ok());
        EXPECT_EQ(gate.enter_commit(instance(), 10).status().code(), StatusCode::Conflict);
        EXPECT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Obsolete);
    }
    auto cut = std::async(std::launch::async, [&] { return gate.close_and_capture(instance()); });
    const auto waiting = cut.wait_for(10ms);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        condition.notify_all();
    }
    EXPECT_EQ(fenced, std::future_status::ready);
    EXPECT_EQ(waiting, std::future_status::timeout);
    EXPECT_TRUE(producer.get());
    const auto capture_result = cut.get();
    ASSERT_TRUE(capture_result.ok());
    const auto captured = capture_result.value();
    EXPECT_TRUE(captured.control_fence);
    EXPECT_FALSE(captured.open);
    EXPECT_EQ(captured.commit_inflight, 0);
    EXPECT_EQ(captured.accepted_records, 1);
}

TEST(AdmissionGate, GuardMovesAndRejectedAttemptsNeverDuplicateAcceptedAccounting) {
    pipeline::AdmissionGate first(instance(), 10);
    pipeline::AdmissionGate second(instance(2), 10);
    ASSERT_EQ(first.open_admission(instance(), 10), pipeline::GateOpenResult::Opened);
    ASSERT_EQ(second.open_admission(instance(2), 10), pipeline::GateOpenResult::Opened);
    {
        auto abandoned = first.enter_commit(instance(), 10);
        ASSERT_TRUE(abandoned.ok());
    }
    EXPECT_EQ(first.snapshot().accepted_records, 0);
    {
        auto entered = first.enter_commit(instance(), 10);
        ASSERT_TRUE(entered.ok());
        auto original = std::move(entered).value();
        pipeline::AdmissionCommit moved(std::move(original));
        EXPECT_FALSE(original.mark_committed());
        EXPECT_TRUE(moved.mark_committed());
        auto other = second.enter_commit(instance(2), 10);
        ASSERT_TRUE(other.ok());
        moved = std::move(other).value();
        EXPECT_TRUE(moved.mark_committed());
    }
    EXPECT_EQ(first.close_and_capture(instance()).value().accepted_records, 1);
    EXPECT_EQ(second.close_and_capture(instance(2)).value().accepted_records, 1);
}

TEST(AdmissionGate, ConcurrentProducersAndRepeatedLateOpenConvergeToFrozenCut) {
    pipeline::AdmissionGate gate(instance(), 10);
    ASSERT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Opened);
    std::atomic<bool> start{false};
    std::atomic<unsigned> accepted{0};
    std::atomic<unsigned> ready{0};
    std::vector<std::future<void>> producers;
    for (unsigned thread = 0; thread < 4; ++thread) {
        producers.push_back(std::async(std::launch::async, [&] {
            ++ready;
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (unsigned attempt = 0; attempt < 1000; ++attempt) {
                auto commit = gate.enter_commit(instance(), 10);
                if (commit.ok() && commit.value().mark_committed()) {
                    ++accepted;
                }
            }
        }));
    }
    while (ready != 4) {
        std::this_thread::yield();
    }
    start = true;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (accepted < 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_GE(accepted.load(), 4);
    ASSERT_TRUE(gate.establish_control_fence(instance()).ok());
    const auto captured = gate.close_and_capture(instance());
    ASSERT_TRUE(captured.ok());
    const auto cut = captured.value();
    for (auto& producer : producers) {
        producer.get();
    }
    EXPECT_EQ(cut.accepted_records, accepted.load());
    EXPECT_EQ(gate.snapshot().accepted_records, accepted.load());
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        EXPECT_EQ(gate.open_admission(instance(), 10), pipeline::GateOpenResult::Obsolete);
        EXPECT_EQ(gate.enter_commit(instance(), 10).status().code(), StatusCode::Conflict);
    }
}

TEST(AdmissionGate, InvalidInstanceCannotOpenAndFreshInstanceIdsUseCanonicalOwnedUuidPrimitive) {
    pipeline::AdmissionGate invalid({}, 10);
    pipeline::AdmissionGate zero_generation(instance(), 0);
    EXPECT_EQ(invalid.open_admission({}, 10), pipeline::GateOpenResult::Obsolete);
    EXPECT_EQ(zero_generation.open_admission(instance(), 0), pipeline::GateOpenResult::Obsolete);
    EXPECT_TRUE(invalid.snapshot().control_fence);
    std::set<std::string> identities;
    for (unsigned count = 0; count < 128; ++count) {
        auto created = pipeline::new_instance_id();
        ASSERT_TRUE(created.ok());
        EXPECT_TRUE(pipeline::valid_instance_id(created.value()));
        const auto text = pipeline::to_string(created.value());
        EXPECT_TRUE(identities.insert(text).second);
        auto parsed = pipeline::parse_instance_id(text);
        ASSERT_TRUE(parsed.ok());
        EXPECT_EQ(parsed.value(), created.value());
    }
    EXPECT_FALSE(pipeline::parse_instance_id("00000000-0000-0000-0000-000000000000").ok());
}

}  // namespace
