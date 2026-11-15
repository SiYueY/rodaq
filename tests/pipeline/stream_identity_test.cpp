#include "pipeline/stream_identity_store.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

namespace pipeline = rodaq::pipeline;
namespace persistence = rodaq::persistence;
using rodaq::StatusCode;
using namespace std::chrono_literals;

class StreamIdentityStoreTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-stream-state-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        task_id_ = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        projection_ = {"sha256:" + std::string(64, 'a'), 10, 30, {{20, "a"}, {30, "b"}}};
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    auto open(
        pipeline::IdentityStoreMode mode = pipeline::IdentityStoreMode::Materializing,
        persistence::FileFaultHook faults = {}, std::size_t capacity = 8) {
        return pipeline::StreamIdentityStore::open(
            root_, task_id_, mode, capacity, std::move(faults));
    }

    persistence::Json document() {
        auto bytes = persistence::read_file(root_, rodaq::to_string(task_id_) + ".json", 32768);
        EXPECT_TRUE(bytes.ok());
        return persistence::Json::parse(bytes.value());
    }

    void replace_document(const persistence::Json& document, bool integrity = true) {
        auto bytes = integrity ? persistence::encode_integrity(document, "rodaq.stream-identity.v1")
                               : rodaq::Result<std::string>{document.dump()};
        ASSERT_TRUE(bytes.ok());
        auto written = persistence::publish_file(
            root_, rodaq::to_string(task_id_) + ".json", bytes.value(),
            persistence::FilePublishMode::Replace, 32768);
        ASSERT_TRUE(written.status.ok());
    }

    std::filesystem::path root_;
    rodaq::TaskId task_id_;
    pipeline::StreamIdentityProjection projection_;
};

TEST_F(StreamIdentityStoreTest, InitialAuthorityGoldenAndRestartAbandonReservedTailWithoutReuse) {
    EXPECT_EQ(open(pipeline::IdentityStoreMode::Existing).status().code(), StatusCode::DataLoss);
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened).value();
    EXPECT_EQ(open().status().code(), StatusCode::Conflict);
    ASSERT_TRUE(store->initialize(projection_).ok());
    EXPECT_EQ(
        document().at("content_digest"),
        "sha256:3353e54f74b929f572232a56c32b25b0f19b9959307be6fb36bc770a09528884");
    auto range = store->reserve(20, 10);
    ASSERT_TRUE(range.ok());
    EXPECT_EQ(range.value().begin, 1);
    EXPECT_EQ(range.value().end_exclusive, 11);
    ASSERT_TRUE(store->initialize(projection_).ok());
    EXPECT_EQ(store->load().value().streams[0].reserved_end_exclusive, 11);
    store.reset();
    auto reopened = open(pipeline::IdentityStoreMode::Existing);
    ASSERT_TRUE(reopened.ok());
    auto next = reopened.value()->reserve(20, 5);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value().begin, 11);
    EXPECT_EQ(next.value().end_exclusive, 16);
    EXPECT_EQ(reopened.value()->reserve(30, 3).value().begin, 1);
}

TEST_F(
    StreamIdentityStoreTest,
    UnknownWriteOutcomeBurnsActualRangeBeforeRetryAndNeverReturnsItAsSuccess) {
    std::atomic<bool> fail{false};
    auto opened = open(pipeline::IdentityStoreMode::Materializing, [&](persistence::FileStep step) {
        return fail && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.initialize(projection_).ok());
    fail = true;
    EXPECT_FALSE(store.reserve(20, 10).ok());
    EXPECT_EQ(store.reserve(20, 10).status().code(), StatusCode::Unavailable);
    EXPECT_FALSE(store.reconcile().ok());
    fail = false;
    auto reconciled = store.reconcile();
    ASSERT_TRUE(reconciled.ok());
    ASSERT_TRUE(reconciled.value());
    EXPECT_EQ(reconciled.value()->streams[0].reserved_end_exclusive, 11);
    auto retry = store.reserve(20, 10);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value().begin, 11);
}

TEST_F(StreamIdentityStoreTest, FailedWriteBeforeRenameDoesNotAdvanceDurableRange) {
    std::atomic<bool> fail{false};
    auto opened = open(pipeline::IdentityStoreMode::Materializing, [&](persistence::FileStep step) {
        return fail && step == persistence::FileStep::SyncFile ? ENOSPC : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.initialize(projection_).ok());
    fail = true;
    EXPECT_EQ(store.reserve(20, 10).status().code(), StatusCode::ResourceExhausted);
    fail = false;
    ASSERT_TRUE(store.reconcile().ok());
    EXPECT_EQ(store.reserve(20, 10).value().begin, 1);
}

TEST_F(StreamIdentityStoreTest, StreamRetirementAndControlSelectedReprojectionKeepAllHighWater) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.initialize(projection_).ok());
    ASSERT_TRUE(store.reserve(20, 7).ok());
    auto changed = projection_;
    changed.config_version = "sha256:" + std::string(64, 'b');
    changed.pipeline_id_high_water = 11;
    changed.stream_id_high_water = 31;
    changed.streams = {{30, "b"}, {31, "new"}};
    auto before = store.load();
    ASSERT_TRUE(before.ok());
    ASSERT_TRUE(store.apply_config_change(changed, before.value().record_revision).ok());
    auto projected = store.load();
    ASSERT_TRUE(projected.ok());
    EXPECT_TRUE(projected.value().streams[0].retired);
    EXPECT_EQ(projected.value().streams[0].reserved_end_exclusive, 8);
    EXPECT_EQ(store.reserve(20, 1).status().code(), StatusCode::InvalidState);
    // The Configure projection was durable but Task Control did not accept it.
    // Core restores the authoritative old Config with the current identity HWM.
    auto restored = projection_;
    restored.pipeline_id_high_water = projected.value().pipeline_id_high_water;
    restored.stream_id_high_water = projected.value().stream_id_high_water;
    ASSERT_TRUE(store.apply_config_change(restored, projected.value().record_revision).ok());
    auto actual = store.load();
    ASSERT_TRUE(actual.ok());
    EXPECT_EQ(actual.value().pipeline_id_high_water, 11);
    EXPECT_EQ(actual.value().stream_id_high_water, 31);
    EXPECT_EQ(store.reserve(20, 1).value().begin, 8);
    EXPECT_TRUE(actual.value().streams[2].retired);
    EXPECT_EQ(
        store.apply_config_change(changed, before.value().record_revision).status().code(),
        StatusCode::Conflict);
    auto rebound = restored;
    rebound.streams[0].stream_key = "other-binding";
    EXPECT_EQ(
        store.apply_config_change(rebound, store.load().value().record_revision).status().code(),
        StatusCode::Conflict);
}

TEST_F(
    StreamIdentityStoreTest,
    RetainedIdentityBudgetIncludesRetiredFactsAndCannotRecycleUnknownOldId) {
    auto opened = open(pipeline::IdentityStoreMode::Materializing, {}, 2);
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.initialize(projection_).ok());
    auto changed = projection_;
    changed.config_version = "sha256:" + std::string(64, 'b');
    changed.stream_id_high_water = 31;
    changed.streams = {{30, "b"}, {31, "new"}};
    EXPECT_EQ(store.apply_config_change(changed, 1).status().code(), StatusCode::ResourceExhausted);
    changed.streams = {{19, "old-hole"}, {30, "b"}};
    EXPECT_EQ(store.apply_config_change(changed, 1).status().code(), StatusCode::Conflict);
    EXPECT_EQ(store.load().value().record_revision, 1);
}

TEST_F(
    StreamIdentityStoreTest,
    FullWidthSequenceBoundaryAllocatesMaximumMinusOneThenPermanentlyRejects) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened).value();
    ASSERT_TRUE(store->initialize(projection_).ok());
    auto near_limit = document();
    near_limit["streams"][0]["reserved_end_exclusive"] = "18446744073709551614";
    store.reset();
    replace_document(near_limit);
    auto reopened = open(pipeline::IdentityStoreMode::Existing);
    ASSERT_TRUE(reopened.ok());
    auto& current = *reopened.value();
    auto final = current.reserve(20, 1);
    ASSERT_TRUE(final.ok());
    EXPECT_EQ(final.value().begin, std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_EQ(final.value().end_exclusive, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(current.reserve(20, 1).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(
        current.reserve(20, std::numeric_limits<std::uint32_t>::max()).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(current.reserve(0, 1).status().code(), StatusCode::InvalidArgument);
    EXPECT_EQ(current.reserve(30, 0).status().code(), StatusCode::InvalidArgument);
}

TEST_F(StreamIdentityStoreTest, ConcurrentRefillsCannotLoseAnotherStreamsReadModifyWrite) {
    std::mutex mutex;
    std::condition_variable condition;
    bool block = false;
    bool entered = false;
    bool release = false;
    auto opened = open(pipeline::IdentityStoreMode::Materializing, [&](persistence::FileStep step) {
        std::unique_lock<std::mutex> lock(mutex);
        if (block && step == persistence::FileStep::SyncDirectory) {
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        return 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.initialize(projection_).ok());
    block = true;
    auto first = std::async(std::launch::async, [&] { return store.reserve(20, 10); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 5s, [&] { return entered; }));
    }
    auto second = std::async(std::launch::async, [&] { return store.reserve(30, 20); });
    const auto ready = second.wait_for(1s);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        condition.notify_all();
    }
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_EQ(second.get().status().code(), StatusCode::Conflict);
    EXPECT_TRUE(first.get().ok());
    ASSERT_TRUE(store.reserve(30, 20).ok());
    auto actual = store.load();
    ASSERT_TRUE(actual.ok());
    EXPECT_EQ(actual.value().streams[0].reserved_end_exclusive, 11);
    EXPECT_EQ(actual.value().streams[1].reserved_end_exclusive, 21);
}

TEST_F(
    StreamIdentityStoreTest,
    SchemaDigestAndKnownCandidateChecksFailClosedWithoutResettingIdentity) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened).value();
    ASSERT_TRUE(store->initialize(projection_).ok());
    const auto original = document();
    auto changed = original;
    changed["streams"][0]["reserved_end_exclusive"] = "99";
    replace_document(changed);
    EXPECT_EQ(store->load().status().code(), StatusCode::DataLoss);
    EXPECT_EQ(store->reconcile().status().code(), StatusCode::DataLoss);
    EXPECT_EQ(store->initialize(projection_).status().code(), StatusCode::Unavailable);
    store.reset();
    for (const auto* field :
         {"stream_id", "reserved_end_exclusive", "retired", "retired_at_config_version"}) {
        auto missing = original;
        missing["streams"][0].erase(field);
        replace_document(missing);
        EXPECT_EQ(
            open(pipeline::IdentityStoreMode::Existing).status().code(), StatusCode::DataLoss);
    }
    changed = original;
    changed["format_version"] = "2.0";
    replace_document(changed);
    EXPECT_EQ(open(pipeline::IdentityStoreMode::Existing).status().code(), StatusCode::Unavailable);
    changed = original;
    changed["streams"][0]["reserved_end_exclusive"] = 1;
    replace_document(changed);
    EXPECT_EQ(open(pipeline::IdentityStoreMode::Existing).status().code(), StatusCode::DataLoss);
    changed = original;
    changed["streams"][0]["retired"] = true;
    replace_document(changed, false);
    EXPECT_EQ(open(pipeline::IdentityStoreMode::Existing).status().code(), StatusCode::DataLoss);
}

TEST_F(
    StreamIdentityStoreTest, ProcessExitAfterRangeRenameBurnsTailWhenRestartProvesActualAuthority) {
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        bool armed = false;
        auto opened =
            open(pipeline::IdentityStoreMode::Materializing, [&](persistence::FileStep step) {
                if (armed && step == persistence::FileStep::SyncDirectory) {
                    _exit(91);
                }
                return 0;
            });
        if (!opened.ok() || !opened.value()->initialize(projection_).ok()) {
            _exit(90);
        }
        armed = true;
        (void)opened.value()->reserve(20, 100);
        _exit(90);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 91);
    auto recovered = open(pipeline::IdentityStoreMode::Existing);
    ASSERT_TRUE(recovered.ok());
    auto next = recovered.value()->reserve(20, 1);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value().begin, 101);
}

TEST_F(
    StreamIdentityStoreTest,
    DescriptorIdsReserveIndependentlyAndSurviveRestartRetirementAndProjection) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened).value();
    ASSERT_TRUE(store->initialize(projection_).ok());
    auto descriptors = store->reserve_descriptors(20, 8);
    ASSERT_TRUE(descriptors.ok());
    EXPECT_EQ(descriptors.value().begin, 1);
    EXPECT_EQ(descriptors.value().end_exclusive, 9);
    EXPECT_EQ(store->reserve(20, 3).value().begin, 1);
    auto projected = projection_;
    projected.config_version = "sha256:" + std::string(64, 'b');
    projected.streams.erase(projected.streams.begin());
    ASSERT_TRUE(store->apply_config_change(projected, store->load().value().record_revision).ok());
    EXPECT_EQ(store->reserve_descriptors(20, 1).status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(
        store->apply_config_change(projection_, store->load().value().record_revision).ok());
    store.reset();
    auto recovered = open(pipeline::IdentityStoreMode::Existing);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value()->reserve_descriptors(20, 1).value().begin, 9);
    EXPECT_EQ(recovered.value()->reserve(20, 1).value().begin, 4);
    EXPECT_EQ(recovered.value()->reserve_descriptors(30, 1).value().begin, 1);
}

TEST_F(
    StreamIdentityStoreTest, UnchangedProjectionIsIdempotentAndVersionCannotNameDifferentBindings) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.initialize(projection_).ok());
    ASSERT_TRUE(store.apply_config_change(projection_, 1).ok());
    EXPECT_EQ(store.load().value().record_revision, 1);
    auto invalid = projection_;
    invalid.streams.erase(invalid.streams.begin());
    EXPECT_EQ(store.apply_config_change(invalid, 1).status().code(), StatusCode::Conflict);
    EXPECT_EQ(store.load().value().record_revision, 1);
}

}  // namespace
