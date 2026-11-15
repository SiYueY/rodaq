#include "persistence/atomic_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using persistence::FileDisposition;
using persistence::FilePublishMode;
using persistence::FileStep;

class AtomicFile : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-atomic-file-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    persistence::FilePublication write(
        std::string_view contents, FilePublishMode mode,
        const persistence::FileFaultHook& faults = {}) const {
        return persistence::publish_file(root_, "state.json", contents, mode, 1024 * 1024, faults);
    }

    fs::path root_;
};

TEST_F(AtomicFile, CreateReplaceAndNoReplaceConflictPreserveWholeAuthority) {
    const auto created = write("old", FilePublishMode::Create);
    ASSERT_TRUE(created.status.ok());
    EXPECT_EQ(created.disposition, FileDisposition::Durable);
    EXPECT_EQ(created.step, FileStep::Complete);
    EXPECT_TRUE(created.retained_temporary.empty());
    const auto conflict = write("must not overwrite", FilePublishMode::Create);
    EXPECT_EQ(conflict.status.code(), rodaq::StatusCode::Conflict);
    EXPECT_EQ(conflict.disposition, FileDisposition::NotPublished);
    EXPECT_TRUE(conflict.retained_temporary.empty());
    EXPECT_EQ(persistence::read_file(root_, "state.json", 64).value(), "old");
    const auto replaced = write("new", FilePublishMode::Replace);
    ASSERT_TRUE(replaced.status.ok());
    EXPECT_EQ(replaced.disposition, FileDisposition::Durable);
    EXPECT_EQ(persistence::read_file(root_, "state.json", 64).value(), "new");
    EXPECT_EQ(std::distance(fs::directory_iterator(root_), fs::directory_iterator()), 1);
}

TEST_F(AtomicFile, FailuresBeforePublicationKeepOldAuthorityAndReleaseTemporaryOwnership) {
    ASSERT_TRUE(write("old", FilePublishMode::Create).status.ok());
    for (const auto step :
         {FileStep::OpenDirectory, FileStep::CreateTemporary, FileStep::WriteTemporary,
          FileStep::SyncFile, FileStep::Publish}) {
        SCOPED_TRACE(static_cast<int>(step));
        const auto failed = write("new", FilePublishMode::Replace, [step](FileStep current) {
            return current == step ? EIO : 0;
        });
        EXPECT_FALSE(failed.status.ok());
        EXPECT_EQ(failed.disposition, FileDisposition::NotPublished);
        EXPECT_EQ(failed.step, step);
        EXPECT_EQ(failed.system_error, EIO);
        EXPECT_EQ(failed.cleanup_error, 0);
        EXPECT_TRUE(failed.retained_temporary.empty());
        EXPECT_EQ(persistence::read_file(root_, "state.json", 64).value(), "old");
        EXPECT_EQ(std::distance(fs::directory_iterator(root_), fs::directory_iterator()), 1);
    }
}

TEST_F(AtomicFile, ParentSyncFailureNeverClaimsDurableDespiteVisibleNewFile) {
    ASSERT_TRUE(write("old", FilePublishMode::Create).status.ok());
    const auto failed = write("new", FilePublishMode::Replace, [](FileStep step) {
        return step == FileStep::SyncDirectory ? EIO : 0;
    });
    EXPECT_FALSE(failed.status.ok());
    EXPECT_EQ(failed.disposition, FileDisposition::Uncertain);
    EXPECT_EQ(failed.step, FileStep::SyncDirectory);
    EXPECT_EQ(failed.system_error, EIO);
    EXPECT_TRUE(failed.retained_temporary.empty());
    EXPECT_EQ(persistence::read_file(root_, "state.json", 64).value(), "new");
}

TEST_F(AtomicFile, CleanupFailurePreservesPrimaryAndReturnsRemainingTemporaryOwner) {
    ASSERT_TRUE(write("old", FilePublishMode::Create).status.ok());
    const auto failed = write("new", FilePublishMode::Replace, [](FileStep step) {
        if (step == FileStep::WriteTemporary) {
            return ENOSPC;
        }
        return step == FileStep::RemoveTemporary ? EACCES : 0;
    });
    EXPECT_EQ(failed.status.code(), rodaq::StatusCode::ResourceExhausted);
    EXPECT_EQ(failed.disposition, FileDisposition::NotPublished);
    EXPECT_EQ(failed.system_error, ENOSPC);
    EXPECT_EQ(failed.cleanup_error, EACCES);
    ASSERT_FALSE(failed.retained_temporary.empty());
    EXPECT_TRUE(fs::is_regular_file(root_ / failed.retained_temporary));
    EXPECT_EQ(persistence::read_file(root_, "state.json", 64).value(), "old");
    EXPECT_TRUE(fs::remove(root_ / failed.retained_temporary));
}

TEST_F(AtomicFile, InvalidNamesSymlinksFifosAndLimitsFailClosedWithoutBlocking) {
    for (const std::string_view name : std::array<std::string_view, 6>{
             "", ".", "..", "../escape", "sub/name", std::string_view("a\0b", 3)}) {
        EXPECT_EQ(
            persistence::publish_file(root_, name, "{}", FilePublishMode::Create, 8).status.code(),
            rodaq::StatusCode::InvalidArgument);
        EXPECT_FALSE(persistence::read_file(root_, name, 8).ok());
    }
    EXPECT_EQ(
        persistence::publish_file(root_, "too-large", "123", FilePublishMode::Create, 2)
            .status.code(),
        rodaq::StatusCode::InvalidArgument);
    EXPECT_TRUE(fs::is_empty(root_));
    ASSERT_TRUE(write("123456", FilePublishMode::Create).status.ok());
    EXPECT_FALSE(persistence::read_file(root_, "state.json", 5).ok());
    fs::create_symlink("state.json", root_ / "link");
    EXPECT_FALSE(persistence::read_file(root_, "link", 64).ok());
    ASSERT_EQ(mkfifo((root_ / "fifo").c_str(), 0600), 0);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(persistence::read_file(root_, "fifo", 64).ok());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
    EXPECT_EQ(
        persistence::read_file(root_, "missing", 64).status().code(), rodaq::StatusCode::NotFound);
}

TEST_F(AtomicFile, ConcurrentCreateHasExactlyOneWinnerAndNoLostTemporaryOwnership) {
    auto left =
        std::async(std::launch::async, [&] { return write("left", FilePublishMode::Create); });
    auto right =
        std::async(std::launch::async, [&] { return write("right", FilePublishMode::Create); });
    const auto first = left.get();
    const auto second = right.get();
    ASSERT_NE(first.status.ok(), second.status.ok());
    EXPECT_EQ(
        first.status.ok() ? second.status.code() : first.status.code(),
        rodaq::StatusCode::Conflict);
    const auto read = persistence::read_file(root_, "state.json", 64);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value(), first.status.ok() ? "left" : "right");
    EXPECT_TRUE(first.retained_temporary.empty());
    EXPECT_TRUE(second.retained_temporary.empty());
    EXPECT_EQ(std::distance(fs::directory_iterator(root_), fs::directory_iterator()), 1);
}

TEST_F(AtomicFile, ReadersKeepACompleteOldOrNewInodeDuringAtomicReplacement) {
    const std::string old(65536, 'a');
    const std::string next(65536, 'b');
    ASSERT_TRUE(write(old, FilePublishMode::Create).status.ok());
    auto reader = std::async(std::launch::async, [&] {
        for (unsigned int iteration = 0; iteration < 1000; ++iteration) {
            const auto read = persistence::read_file(root_, "state.json", old.size());
            if (!read.ok() || (read.value() != old && read.value() != next)) {
                return false;
            }
        }
        return true;
    });
    for (unsigned int iteration = 0; iteration < 50; ++iteration) {
        ASSERT_TRUE(write(iteration % 2 == 0 ? next : old, FilePublishMode::Replace).status.ok());
    }
    EXPECT_TRUE(reader.get());
}

}  // namespace
