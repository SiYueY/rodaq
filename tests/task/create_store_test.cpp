#include "task/create_store.hpp"
#include "task/identity.hpp"
#include "persistence/json.hpp"
#include "time/wall_time.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

namespace task = rodaq::task;
namespace persistence = rodaq::persistence;
using rodaq::StatusCode;
using rodaq::WallTime;
using namespace std::chrono_literals;

class TaskCreateStoreTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-create-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        key_ = task::request_key_hash({"create-key"}).value();
        digest_ = "sha256:" + std::string(64, 'a');
        first_ = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        second_ = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ac").value();
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    auto open(std::size_t capacity = 8, persistence::FileFaultHook faults = {}) {
        return task::TaskCreateStore::open(root_, capacity, std::move(faults));
    }

    std::string file_name() const { return key_.substr(7) + ".json"; }

    persistence::Json document() {
        auto bytes = persistence::read_file(root_, file_name(), 2048);
        EXPECT_TRUE(bytes.ok());
        return persistence::Json::parse(bytes.value());
    }

    void replace(persistence::Json document, bool integrity = true) {
        auto bytes = integrity ? persistence::encode_integrity(document, "rodaq.create-receipt.v1")
                               : rodaq::Result<std::string>{document.dump()};
        ASSERT_TRUE(bytes.ok());
        auto written = persistence::publish_file(
            root_, file_name(), bytes.value(), persistence::FilePublishMode::Replace, 2048);
        ASSERT_TRUE(written.status.ok());
    }

    std::filesystem::path root_;
    std::string key_;
    std::string digest_;
    rodaq::TaskId first_;
    rodaq::TaskId second_;
};

TEST(WallTime, ExactNanosecondUtcRoundTripAndRangeChecks) {
    for (const auto count :
         {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1000000001}, std::int64_t{-1},
          std::int64_t{0}, std::int64_t{1}, std::numeric_limits<std::int64_t>::max()}) {
        const WallTime value{std::chrono::nanoseconds{count}};
        auto text = rodaq::time::format_wall_time(value);
        ASSERT_TRUE(text.ok());
        auto parsed = rodaq::time::parse_wall_time(text.value());
        ASSERT_TRUE(parsed.ok()) << text.value();
        EXPECT_EQ(parsed.value(), value);
    }
    EXPECT_EQ(
        rodaq::time::format_wall_time(WallTime{-1ns}).value(), "1969-12-31T23:59:59.999999999Z");
    for (const auto* text :
         {"2025-02-29T00:00:00.000000000Z", "2024-04-31T00:00:00.000000000Z",
          "2024-01-01T24:00:00.000000000Z", "2024-01-01T00:00:60.000000000Z",
          "2262-04-11T23:47:16.854775808Z", "1677-09-21T00:12:43.145224191Z",
          "2024-01-01T00:00:00Z", "2024-01-01T00:00:00.000000000z"}) {
        EXPECT_FALSE(rodaq::time::parse_wall_time(text).ok()) << text;
    }
    EXPECT_TRUE(rodaq::time::parse_wall_time("2024-02-29T00:00:00.000000000Z").ok());
}

TEST_F(TaskCreateStoreTest, DurableClaimRetriesAndAllThreeCrashResumePhasesKeepOriginalIdentity) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened).value();
    EXPECT_EQ(open().status().code(), StatusCode::Conflict);
    auto claim = store->claim(key_, digest_, first_, WallTime{123ns});
    ASSERT_TRUE(claim.ok());
    EXPECT_EQ(claim.value().state, task::CreateClaimState::Claimed);
    EXPECT_EQ(
        document().at("content_digest"),
        "sha256:778546d50c321800d4076dc7e4f885137471c1300410280921e100f89d3cff62");
    auto receipt = claim.value().receipt;
    for (auto phase :
         {task::CreatePhase::Allocated, task::CreatePhase::Materializing,
          task::CreatePhase::Completed}) {
        receipt.phase = phase;
        ASSERT_TRUE(store->publish(receipt).ok());
        store.reset();
        auto reopened = open();
        ASSERT_TRUE(reopened.ok());
        store = std::move(reopened).value();
        auto retry = store->claim(key_, digest_, second_, WallTime{999ns});
        ASSERT_TRUE(retry.ok());
        EXPECT_EQ(retry.value().receipt.assigned_task_id, first_);
        EXPECT_EQ(retry.value().receipt.created_at, WallTime{123ns});
        EXPECT_EQ(retry.value().receipt.phase, phase);
        EXPECT_EQ(
            retry.value().state, phase == task::CreatePhase::Completed
                                     ? task::CreateClaimState::ExistingCompleted
                                     : task::CreateClaimState::ExistingInProgress);
    }
    EXPECT_EQ(
        store->claim(key_, "sha256:" + std::string(64, 'b'), second_, {}).status().code(),
        StatusCode::Conflict);
}

TEST_F(TaskCreateStoreTest, CapacityIdCollisionsAndPhaseRegressionsFailBeforePublication) {
    auto opened = open(2);
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    auto claim = store.claim(key_, digest_, first_, {});
    ASSERT_TRUE(claim.ok());
    auto receipt = claim.value().receipt;
    receipt.phase = task::CreatePhase::Completed;
    EXPECT_EQ(store.publish(receipt).status().code(), StatusCode::Conflict);
    receipt.phase = task::CreatePhase::Materializing;
    ASSERT_TRUE(store.publish(receipt).ok());
    receipt.phase = task::CreatePhase::Allocated;
    EXPECT_EQ(store.publish(receipt).status().code(), StatusCode::Conflict);
    receipt.phase = task::CreatePhase::Materializing;
    receipt.assigned_task_id = second_;
    EXPECT_EQ(store.publish(receipt).status().code(), StatusCode::Conflict);
    const auto other = task::request_key_hash({"second"}).value();
    EXPECT_EQ(store.claim(other, digest_, first_, {}).status().code(), StatusCode::Conflict);
    ASSERT_TRUE(store.claim(other, digest_, second_, {}).ok());
    EXPECT_EQ(
        store.claim(task::request_key_hash({"third"}).value(), digest_, second_, {})
            .status()
            .code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(
        store.claim(std::string(100000, 'a'), digest_, first_, {}).status().code(),
        StatusCode::InvalidArgument);
}

TEST_F(
    TaskCreateStoreTest,
    PrePublishFailureDoesNotAllocateWhileUncertainClaimRequiresActualReconciliation) {
    std::atomic<int> failure{0};
    auto opened = open(8, [&](persistence::FileStep step) {
        if (failure == 1 && step == persistence::FileStep::WriteTemporary) {
            return ENOSPC;
        }
        return failure == 2 && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    failure = 1;
    EXPECT_FALSE(store.claim(key_, digest_, first_, {}).ok());
    EXPECT_FALSE(std::filesystem::exists(root_ / file_name()));
    failure = 2;
    EXPECT_FALSE(store.claim(key_, digest_, first_, {}).ok());
    EXPECT_TRUE(std::filesystem::exists(root_ / file_name()));
    EXPECT_EQ(store.claim(key_, digest_, second_, {}).status().code(), StatusCode::Unavailable);
    EXPECT_FALSE(store.reconcile().ok());
    failure = 0;
    ASSERT_TRUE(store.reconcile().ok());
    auto retry = store.claim(key_, digest_, second_, {});
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value().receipt.assigned_task_id, first_);
    EXPECT_EQ(retry.value().state, task::CreateClaimState::ExistingInProgress);
}

TEST_F(TaskCreateStoreTest, UncertainPhaseUpdateDoesNotRevertActualCompletedReceipt) {
    std::atomic<bool> fail{false};
    auto opened = open(8, [&](persistence::FileStep step) {
        return fail && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    auto claim = store.claim(key_, digest_, first_, {});
    ASSERT_TRUE(claim.ok());
    auto receipt = claim.value().receipt;
    receipt.phase = task::CreatePhase::Materializing;
    ASSERT_TRUE(store.publish(receipt).ok());
    receipt.phase = task::CreatePhase::Completed;
    fail = true;
    EXPECT_FALSE(store.publish(receipt).ok());
    EXPECT_FALSE(store.publish(receipt).ok());
    fail = false;
    ASSERT_TRUE(store.reconcile().ok());
    auto retry = store.claim(key_, digest_, second_, {});
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value().state, task::CreateClaimState::ExistingCompleted);
}

TEST_F(TaskCreateStoreTest, SlowFsyncHoldsSingleFlightAndConcurrentCallerReturnsWithoutQueueing) {
    std::mutex mutex;
    std::condition_variable condition;
    bool block = false;
    bool entered = false;
    bool release = false;
    auto opened = open(8, [&](persistence::FileStep step) {
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
    block = true;
    auto pending =
        std::async(std::launch::async, [&] { return store.claim(key_, digest_, first_, {}); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 5s, [&] { return entered; }));
    }
    auto concurrent =
        std::async(std::launch::async, [&] { return store.claim(key_, digest_, second_, {}); });
    const auto ready = concurrent.wait_for(1s);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        condition.notify_all();
    }
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_EQ(concurrent.get().status().code(), StatusCode::Conflict);
    EXPECT_TRUE(pending.get().ok());
}

TEST_F(TaskCreateStoreTest, ExternalChangesCorruptionUnknownVersionsAndFilenameMismatchFailClosed) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened).value();
    ASSERT_TRUE(store->claim(key_, digest_, first_, {}).ok());
    auto original = document();
    auto modified = original;
    modified["phase"] = "Completed";
    replace(modified);
    EXPECT_EQ(store->claim(key_, digest_, second_, {}).status().code(), StatusCode::Conflict);
    EXPECT_EQ(store->claim(key_, digest_, second_, {}).status().code(), StatusCode::Unavailable);
    store.reset();
    modified = original;
    modified["format_version"] = "2.0";
    replace(modified);
    EXPECT_EQ(open().status().code(), StatusCode::Unavailable);
    modified = original;
    modified["phase"] = "Completed";
    replace(modified, false);
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
    modified = original;
    modified["request_key_hash"] = "sha256:" + std::string(64, 'b');
    replace(modified);
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
    replace(original);
    std::filesystem::rename(root_ / file_name(), root_ / "unknown.json");
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
}

TEST_F(TaskCreateStoreTest, StartupCleansOnlyOwnedTemporaryNamesAndRejectsSymlinkOrFifoRecords) {
    const auto temporary = root_ / (".rodaq-tmp-" + std::string(32, 'a'));
    std::ofstream(temporary) << "unpublished";
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    EXPECT_FALSE(std::filesystem::exists(temporary));
    ASSERT_TRUE(opened.value()->claim(key_, digest_, first_, {}).ok());
    opened.value().reset();
    std::filesystem::rename(root_ / file_name(), root_ / "backing");
    std::filesystem::create_symlink(root_ / "backing", root_ / file_name());
    EXPECT_FALSE(open().ok());
    std::filesystem::remove(root_ / "backing");
    std::filesystem::remove(root_ / file_name());
    ASSERT_EQ(mkfifo((root_ / file_name()).c_str(), 0600), 0);
    EXPECT_FALSE(open().ok());
}

TEST_F(TaskCreateStoreTest, ProcessExitAfterRenameRecoversEachPhaseWithoutSecondTaskIdentity) {
    for (auto phase :
         {task::CreatePhase::Allocated, task::CreatePhase::Materializing,
          task::CreatePhase::Completed}) {
        const auto directory = root_ / std::to_string(static_cast<int>(phase));
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        const auto child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            bool armed = false;
            auto opened =
                task::TaskCreateStore::open(directory, 8, [&](persistence::FileStep step) {
                    if (armed && step == persistence::FileStep::SyncDirectory) {
                        _exit(91);  // Target rename completed; parent fsync has not.
                    }
                    return 0;
                });
            if (!opened.ok()) {
                _exit(90);
            }
            armed = phase == task::CreatePhase::Allocated;
            auto claimed = opened.value()->claim(key_, digest_, first_, {});
            if (!claimed.ok()) {
                _exit(90);
            }
            auto receipt = claimed.value().receipt;
            receipt.phase = task::CreatePhase::Materializing;
            armed = phase == task::CreatePhase::Materializing;
            if (!opened.value()->publish(receipt).ok()) {
                _exit(90);
            }
            receipt.phase = task::CreatePhase::Completed;
            armed = true;
            (void)opened.value()->publish(receipt);
            _exit(90);
        }
        int status = 0;
        ASSERT_EQ(waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 91);
        auto recovered = task::TaskCreateStore::open(directory, 8);
        ASSERT_TRUE(recovered.ok())
            << static_cast<int>(recovered.status().code()) << " " << recovered.status().message();
        auto retry = recovered.value()->claim(key_, digest_, second_, {});
        ASSERT_TRUE(retry.ok());
        EXPECT_EQ(retry.value().receipt.assigned_task_id, first_);
        EXPECT_EQ(retry.value().receipt.phase, phase);
    }
}

}  // namespace
