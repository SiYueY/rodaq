#include "artifact/segment_id_store.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <chrono>
#include <limits>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace artifact = rodaq::artifact;
namespace persistence = rodaq::persistence;
using artifact::SegmentIdOpenMode;
using artifact::SegmentIdStore;
using rodaq::StatusCode;

class SegmentIdStoreTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-segment-ids-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    void replace(std::string end) {
        auto bytes = persistence::encode_integrity(
            {{"format_version", "1.0"}, {"reserved_end_exclusive", std::move(end)}},
            "rodaq.segment-identity.v1");
        ASSERT_TRUE(bytes.ok());
        const auto published = persistence::publish_file(
            root_, "segment-identity.json", bytes.value(), persistence::FilePublishMode::Replace,
            1024);
        ASSERT_TRUE(published.status.ok());
    }

    std::filesystem::path root_;
};

TEST_F(SegmentIdStoreTest, MissingExistingAuthorityFailsClosedAndRestartAbandonsWholeReservedTail) {
    EXPECT_EQ(
        SegmentIdStore::open(root_, SegmentIdOpenMode::Existing).status().code(),
        StatusCode::DataLoss);
    auto opened = SegmentIdStore::open(root_, SegmentIdOpenMode::NewDeployment);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto owner = std::move(opened.value());
    EXPECT_EQ(
        SegmentIdStore::open(root_, SegmentIdOpenMode::Existing).status().code(),
        StatusCode::Conflict);
    auto first = owner->reserve(10);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().begin, 1);
    EXPECT_EQ(first.value().end_exclusive, 11);
    owner.reset();
    opened = SegmentIdStore::open(root_, SegmentIdOpenMode::Existing);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto next = opened.value()->reserve(2);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value().begin, 11);
    EXPECT_EQ(next.value().end_exclusive, 13);
}

TEST_F(SegmentIdStoreTest, UncertainRenameNeverReturnsRangeAndReconciliationBurnsItsCandidate) {
    bool armed = false;
    auto opened = SegmentIdStore::open(
        root_, SegmentIdOpenMode::NewDeployment, [&](persistence::FileStep step) {
            return armed && step == persistence::FileStep::SyncDirectory ? EIO : 0;
        });
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto& owner = opened.value();
    armed = true;
    EXPECT_FALSE(owner->reserve(10).ok());
    EXPECT_EQ(owner->reserve(1).status().code(), StatusCode::Unavailable);
    EXPECT_FALSE(owner->reconcile().ok());
    armed = false;
    ASSERT_TRUE(owner->reconcile().ok());
    auto next = owner->reserve(1);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value().begin, 11);
}

TEST_F(
    SegmentIdStoreTest, FailureBeforePublicationCanReconcileOldAuthorityWithoutInventingUsedIds) {
    bool armed = false;
    auto opened = SegmentIdStore::open(
        root_, SegmentIdOpenMode::NewDeployment, [&](persistence::FileStep step) {
            return armed && step == persistence::FileStep::WriteTemporary ? ENOSPC : 0;
        });
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    armed = true;
    EXPECT_FALSE(opened.value()->reserve(10).ok());
    armed = false;
    ASSERT_TRUE(opened.value()->reconcile().ok());
    auto next = opened.value()->reserve(1);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value().begin, 1);
}

TEST_F(
    SegmentIdStoreTest, CorruptionAndUnexpectedExternalValidAuthorityCannotBeAdoptedByLiveOwner) {
    auto opened = SegmentIdStore::open(root_, SegmentIdOpenMode::NewDeployment);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    replace("42");
    EXPECT_EQ(opened.value()->reserve(1).status().code(), StatusCode::DataLoss);
    EXPECT_EQ(opened.value()->reconcile().status().code(), StatusCode::DataLoss);
    replace("0");
    opened.value().reset();
    EXPECT_EQ(
        SegmentIdStore::open(root_, SegmentIdOpenMode::Existing).status().code(),
        StatusCode::DataLoss);
}

TEST_F(SegmentIdStoreTest, FullWidthHighWaterNeverWrapsAndZeroRangeIsInvalid) {
    auto opened = SegmentIdStore::open(root_, SegmentIdOpenMode::NewDeployment);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    opened.value().reset();
    replace(std::to_string(std::numeric_limits<std::uint64_t>::max() - 1));
    opened = SegmentIdStore::open(root_, SegmentIdOpenMode::Existing);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    EXPECT_EQ(opened.value()->reserve(0).status().code(), StatusCode::InvalidArgument);
    auto last = opened.value()->reserve(1);
    ASSERT_TRUE(last.ok());
    EXPECT_EQ(last.value().begin, std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_EQ(last.value().end_exclusive, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(opened.value()->reserve(1).status().code(), StatusCode::ResourceExhausted);
}

TEST_F(SegmentIdStoreTest, CrashAfterRenameBeforeParentSyncNeverReissuesReservedRange) {
    auto initialized = SegmentIdStore::open(root_, SegmentIdOpenMode::NewDeployment);
    ASSERT_TRUE(initialized.ok());
    initialized.value().reset();
    const auto child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        bool armed = false;
        auto opened = SegmentIdStore::open(
            root_, SegmentIdOpenMode::Existing, [&](persistence::FileStep step) {
                if (armed && step == persistence::FileStep::SyncDirectory) {
                    _exit(73);
                }
                return 0;
            });
        if (!opened.ok()) {
            _exit(74);
        }
        armed = true;
        (void)opened.value()->reserve(10);
        _exit(75);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 73);
    auto recovered = SegmentIdStore::open(root_, SegmentIdOpenMode::Existing);
    ASSERT_TRUE(recovered.ok());
    auto range = recovered.value()->reserve(1);
    ASSERT_TRUE(range.ok());
    EXPECT_EQ(range.value().begin, 11);
}

TEST_F(SegmentIdStoreTest, UnknownNamespaceFilesAndSymlinkAuthorityAreNotSilentlyAdopted) {
    auto opened = SegmentIdStore::open(root_, SegmentIdOpenMode::NewDeployment);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    opened.value().reset();
    ASSERT_EQ(symlink("segment-identity.json", (root_ / "unowned").c_str()), 0);
    EXPECT_EQ(
        SegmentIdStore::open(root_, SegmentIdOpenMode::Existing).status().code(),
        StatusCode::DataLoss);
}

TEST_F(SegmentIdStoreTest, ActualPersistenceFlightExcludesAnotherReserveAndReconciliation) {
    bool armed = false;
    std::promise<void> entered;
    std::promise<void> release;
    auto allowed = release.get_future().share();
    auto opened = SegmentIdStore::open(
        root_, SegmentIdOpenMode::NewDeployment, [&](persistence::FileStep step) {
            if (armed && step == persistence::FileStep::SyncFile) {
                entered.set_value();
                allowed.wait();
            }
            return 0;
        });
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    armed = true;
    auto first = std::async(std::launch::async, [&] { return opened.value()->reserve(2); });
    auto started = entered.get_future();
    const bool inside = started.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    EXPECT_TRUE(inside);
    if (inside) {
        EXPECT_EQ(opened.value()->reserve(1).status().code(), StatusCode::Conflict);
        EXPECT_EQ(opened.value()->reconcile().status().code(), StatusCode::Conflict);
    }
    release.set_value();
    auto result = first.get();
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().begin, 1);
    EXPECT_EQ(result.value().end_exclusive, 3);
}

}  // namespace
