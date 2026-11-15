#include "time/playback_anchor_store.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <string>
#include <vector>
#include <utility>

namespace {

namespace time = rodaq::time;
namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using time::BootArtifactState;
using time::PlaybackAnchorStore;

class PlaybackAnchorStoreTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-time-state-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        auto identity = time::parse_boot_identity("12345678-1234-4234-8234-123456789abc");
        ASSERT_TRUE(identity.ok());
        boot_ = std::move(identity).value();
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    rodaq::Result<std::unique_ptr<PlaybackAnchorStore>> open(
        time::ClockSample sample = {100, 1000, 1},
        time::BootArtifactEvidence evidence = {BootArtifactState::Absent, {}},
        persistence::FileFaultHook faults = {}) {
        return PlaybackAnchorStore::open(root_, boot_, sample, evidence, 2, std::move(faults));
    }

    persistence::Json document() const {
        const auto bytes = persistence::read_file(root_, "playback-anchor.json", 4096);
        EXPECT_TRUE(bytes.ok());
        const auto parsed = persistence::parse_json(bytes.value());
        EXPECT_TRUE(parsed.ok());
        return parsed.value();
    }

    void replace_document(const persistence::Json& value) {
        auto bytes = persistence::encode_integrity(value, "rodaq.playback-anchor.v1");
        ASSERT_TRUE(bytes.ok());
        auto published = persistence::publish_file(
            root_, "playback-anchor.json", bytes.value(), persistence::FilePublishMode::Replace,
            4096);
        ASSERT_TRUE(published.status.ok());
    }

    fs::path root_;
    time::BootIdentity boot_;
};

TEST_F(
    PlaybackAnchorStoreTest, DurableGoldenAndSameBootRestartKeepAnchorButAbandonReservedEpochTail) {
    auto created = open();
    ASSERT_TRUE(created.ok());
    auto owner = std::move(created).value();
    EXPECT_EQ(owner->initial_range().begin, 1);
    EXPECT_EQ(owner->initial_range().end_exclusive, 3);
    EXPECT_EQ(
        document()["content_digest"],
        "sha256:d8caf0bbc5479ba5524ee217f1ebe205f8682b5f2eeb080f0512d23d1a1f3dc8");
    const auto anchor = owner->anchor();
    owner.reset();
    auto restarted = open({900, -12345, 5}, {BootArtifactState::Present, anchor});
    ASSERT_TRUE(restarted.ok());
    EXPECT_EQ(restarted.value()->anchor().monotonic_ns, 100);
    EXPECT_EQ(restarted.value()->anchor().realtime_ns, 1000);
    EXPECT_EQ(restarted.value()->initial_range().begin, 3);
    EXPECT_EQ(restarted.value()->initial_range().end_exclusive, 5);
}

TEST_F(PlaybackAnchorStoreTest, MissingAuthorityWithRecordingFactsNeverResamples) {
    const time::PlaybackAnchor recorded{boot_.id, 100, 1000};
    EXPECT_EQ(
        open({200, 2000, 1}, {BootArtifactState::Present, recorded}).status().code(),
        rodaq::StatusCode::DataLoss);
    EXPECT_EQ(
        open({200, 2000, 1}, {BootArtifactState::Unknown, {}}).status().code(),
        rodaq::StatusCode::Unavailable);
    EXPECT_FALSE(fs::exists(root_ / "playback-anchor.json"));
}

TEST_F(PlaybackAnchorStoreTest, KernelBootChangeCreatesNewAnchorAndEpochScope) {
    auto first = open();
    ASSERT_TRUE(first.ok());
    first.value().reset();
    auto changed = time::parse_boot_identity("abcdefab-1234-4234-8234-123456789abc");
    ASSERT_TRUE(changed.ok());
    ASSERT_NE(changed.value().id, boot_.id);
    auto next = PlaybackAnchorStore::open(
        root_, changed.value(), {10, 9999, 1}, {BootArtifactState::Absent, {}}, 2);
    ASSERT_TRUE(next.ok());
    EXPECT_EQ(next.value()->anchor().boot_id, changed.value().id);
    EXPECT_EQ(next.value()->anchor().monotonic_ns, 10);
    EXPECT_EQ(next.value()->anchor().realtime_ns, 9999);
    EXPECT_EQ(next.value()->initial_range().begin, 1);
}

TEST_F(PlaybackAnchorStoreTest, CorruptionAndConflictingArtifactAnchorProtectWithoutOverwrite) {
    auto created = open();
    ASSERT_TRUE(created.ok());
    const auto anchor = created.value()->anchor();
    created.value().reset();
    auto conflict = open({200, 9999, 1}, {BootArtifactState::Present, {boot_.id, 100, 1001}});
    EXPECT_EQ(conflict.status().code(), rodaq::StatusCode::DataLoss);
    EXPECT_EQ(document()["playback_anchor_realtime"], "1000");
    auto corrupted = document();
    corrupted["playback_anchor_realtime"] = "1001";
    std::ofstream raw(root_ / "playback-anchor.json");
    raw << corrupted.dump();
    raw.close();
    EXPECT_EQ(
        open({200, 9999, 1}, {BootArtifactState::Present, anchor}).status().code(),
        rodaq::StatusCode::DataLoss);
}

TEST_F(
    PlaybackAnchorStoreTest, UncertainFirstPublishIsNotUsableAndRetryReusesVisibleOriginalAnchor) {
    auto failed =
        open({100, 1000, 1}, {BootArtifactState::Absent, {}}, [](persistence::FileStep step) {
            return step == persistence::FileStep::SyncDirectory ? EIO : 0;
        });
    EXPECT_FALSE(failed.ok());
    auto retry = open({999, -999, 1});
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value()->anchor().monotonic_ns, 100);
    EXPECT_EQ(retry.value()->anchor().realtime_ns, 1000);
    EXPECT_EQ(retry.value()->initial_range().begin, 3);
}

TEST_F(PlaybackAnchorStoreTest, ExclusiveLifetimeLockAndConcurrentReservationsPreventReuse) {
    auto created = open();
    ASSERT_TRUE(created.ok());
    auto owner = std::move(created).value();
    EXPECT_EQ(open().status().code(), rodaq::StatusCode::Conflict);
    std::vector<std::future<rodaq::Result<time::EpochRange>>> workers;
    for (unsigned int index = 0; index < 8; ++index) {
        workers.push_back(std::async(std::launch::async, [&] { return owner->reserve_epochs(); }));
    }
    std::vector<std::uint64_t> starts;
    for (auto& worker : workers) {
        auto range = worker.get();
        ASSERT_TRUE(range.ok());
        starts.push_back(range.value().begin);
        EXPECT_EQ(range.value().end_exclusive - range.value().begin, 2);
    }
    std::sort(starts.begin(), starts.end());
    for (std::size_t index = 0; index < starts.size(); ++index) {
        EXPECT_EQ(starts[index], 3 + index * 2);
    }
    EXPECT_EQ(document()["reserved_epoch_end_exclusive"], "19");
}

TEST_F(PlaybackAnchorStoreTest, FailedReserveCannotPublishAgainOrLoseOriginalAnchor) {
    bool fail = false;
    auto created =
        open({100, 1000, 1}, {BootArtifactState::Absent, {}}, [&](persistence::FileStep step) {
            return fail && step == persistence::FileStep::SyncDirectory ? EIO : 0;
        });
    ASSERT_TRUE(created.ok());
    fail = true;
    EXPECT_FALSE(created.value()->reserve_epochs().ok());
    EXPECT_EQ(created.value()->reserve_epochs().status().code(), rodaq::StatusCode::InvalidState);
    EXPECT_EQ(created.value()->anchor().realtime_ns, 1000);
}

TEST_F(PlaybackAnchorStoreTest, UnexpectedChangedAuthorityAndEpochExhaustionNeverOverwriteOrWrap) {
    auto created = open();
    ASSERT_TRUE(created.ok());
    auto modified = document();
    modified["playback_anchor_realtime"] = "1001";
    replace_document(modified);
    EXPECT_EQ(created.value()->reserve_epochs().status().code(), rodaq::StatusCode::DataLoss);
    EXPECT_EQ(document()["playback_anchor_realtime"], "1001");
    created.value().reset();
    modified["reserved_epoch_end_exclusive"] =
        std::to_string(std::numeric_limits<std::uint64_t>::max());
    replace_document(modified);
    EXPECT_EQ(open().status().code(), rodaq::StatusCode::ResourceExhausted);
}

TEST_F(PlaybackAnchorStoreTest, DecimalSchemaAndVersionFailClosedBeforeAnyNewEpochPublish) {
    auto first = open();
    ASSERT_TRUE(first.ok());
    first.value().reset();
    const auto valid = document();
    for (const std::string bad : {"00", "+3", "-1", "18446744073709551616"}) {
        auto invalid = valid;
        invalid["reserved_epoch_end_exclusive"] = bad;
        replace_document(invalid);
        EXPECT_EQ(open().status().code(), rodaq::StatusCode::DataLoss);
    }
    auto unsupported = valid;
    unsupported["format_version"] = "2.0";
    replace_document(unsupported);
    EXPECT_EQ(open().status().code(), rodaq::StatusCode::Unavailable);
}

TEST_F(
    PlaybackAnchorStoreTest, UnpublishedTemporaryIsDiscardedButUnknownNamespaceEntryIsProtected) {
    auto first = open();
    ASSERT_TRUE(first.ok());
    first.value().reset();
    const auto temporary = root_ / (".rodaq-tmp-" + std::string(32, 'a'));
    {
        std::ofstream abandoned(temporary);
        abandoned << "not authority";
    }
    auto recovered = open({200, 9999, 1});
    ASSERT_TRUE(recovered.ok());
    EXPECT_FALSE(fs::exists(temporary));
    EXPECT_EQ(recovered.value()->anchor().realtime_ns, 1000);
    recovered.value().reset();
    {
        std::ofstream unknown(root_ / "unrecognized");
        unknown << "must preserve";
    }
    EXPECT_EQ(open({200, 9999, 1}).status().code(), rodaq::StatusCode::DataLoss);
    EXPECT_TRUE(fs::exists(root_ / "unrecognized"));
}

}  // namespace
