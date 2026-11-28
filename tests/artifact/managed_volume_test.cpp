#include "artifact/managed_volume.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <vector>

namespace {

namespace artifact = rodaq::artifact;
namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using artifact::ManagedVolume;
using artifact::VolumeOpenMode;
using rodaq::StatusCode;

class ManagedVolumeTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-volume-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        parent_ = created;
        root_ = parent_ / "volume";
        ASSERT_TRUE(fs::create_directory(root_));
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(parent_, ignored);
    }

    std::string marker() const {
        auto bytes = persistence::read_file(root_ / ".rodaq", "volume.json", 4096);
        EXPECT_TRUE(bytes.ok());
        return bytes.ok() ? bytes.value() : std::string{};
    }

    fs::path parent_;
    fs::path root_;
};

TEST_F(
    ManagedVolumeTest, InitializationPublishesStableIdentityAndDirectoriesWithoutRecordingFiles) {
    auto opened = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto owner = std::move(opened.value());
    const auto identity = owner->identity();
    const auto bytes = marker();
    EXPECT_TRUE(artifact::parse_volume_id(artifact::to_string(identity)).ok());
    EXPECT_TRUE(fs::is_empty(root_ / ".rodaq/artifacts"));
    EXPECT_FALSE(fs::exists(root_ / ".rodaq/quarantine"));
    EXPECT_TRUE(owner->verify().ok());
    EXPECT_TRUE(owner->available_bytes().ok());
    EXPECT_NE(owner->runtime_identity().mount_id, 0);
    EXPECT_FALSE(owner->runtime_identity().filesystem_type.empty());
    EXPECT_FALSE(owner->runtime_identity().mount_point.empty());
    EXPECT_EQ(
        ManagedVolume::open(root_, VolumeOpenMode::Existing, identity).status().code(),
        StatusCode::Conflict);
    owner.reset();
    auto reopened = ManagedVolume::open(root_, VolumeOpenMode::Existing, identity);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_EQ(reopened.value()->identity(), identity);
    EXPECT_EQ(marker(), bytes);
}

TEST_F(ManagedVolumeTest, ExistingMissingOrMismatchedMarkerNeverCreatesNewIdentity) {
    const auto identity = artifact::parse_volume_id("01234567-89ab-4cde-8fab-0123456789ab").value();
    EXPECT_EQ(
        ManagedVolume::open(root_, VolumeOpenMode::Existing, identity).status().code(),
        StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(root_ / ".rodaq"));
    auto opened = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok());
    const auto bytes = marker();
    opened.value().reset();
    EXPECT_EQ(
        ManagedVolume::open(root_, VolumeOpenMode::Existing, identity).status().code(),
        StatusCode::Conflict);
    EXPECT_EQ(marker(), bytes);
    ASSERT_TRUE(fs::remove(root_ / ".rodaq/volume.json"));
    EXPECT_EQ(
        ManagedVolume::open(root_, VolumeOpenMode::Initialize, identity).status().code(),
        StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(root_ / ".rodaq/volume.json"));
}

TEST_F(ManagedVolumeTest, LostMarkerProtectsPermanentlyEvenWhenItsOriginalBytesReturn) {
    auto opened = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok());
    const auto bytes = marker();
    ASSERT_TRUE(fs::remove(root_ / ".rodaq/volume.json"));
    EXPECT_FALSE(opened.value()->verify().ok());
    std::ofstream restored(root_ / ".rodaq/volume.json");
    restored << bytes;
    restored.close();
    EXPECT_NE(opened.value()->protection(), StatusCode::Ok);
    EXPECT_FALSE(opened.value()->verify().ok());
    EXPECT_FALSE(opened.value()->create_staging(1).ok());
}

TEST_F(ManagedVolumeTest, ReplacedRootOrManagedDirectoryIsRejectedEvenWithCopiedValidMarker) {
    auto opened = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok());
    fs::rename(root_, parent_ / "old");
    fs::copy(parent_ / "old", root_, fs::copy_options::recursive);
    EXPECT_EQ(opened.value()->verify().status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(opened.value()->create_staging(1).ok());
    opened.value().reset();
    auto second = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(second.ok());
    fs::rename(root_ / ".rodaq/artifacts", root_ / ".rodaq/old-artifacts");
    ASSERT_TRUE(fs::create_directory(root_ / ".rodaq/artifacts"));
    EXPECT_EQ(second.value()->verify().status().code(), StatusCode::DataLoss);
}

TEST_F(ManagedVolumeTest, RedirectedConfiguredAliasCannotContinueUsingItsFormerTarget) {
    const auto alias = parent_ / "alias";
    fs::create_directory_symlink(root_, alias);
    auto opened = ManagedVolume::open(alias, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok());
    fs::copy(root_, parent_ / "replacement", fs::copy_options::recursive);
    ASSERT_TRUE(fs::remove(alias));
    fs::create_directory_symlink(parent_ / "replacement", alias);
    EXPECT_EQ(opened.value()->verify().status().code(), StatusCode::DataLoss);
}

TEST_F(ManagedVolumeTest, StagingCreationIsLazyNoReplaceAndCannotAdoptUnknownDirectory) {
    auto opened = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(opened.value()->create_staging(0).status().code(), StatusCode::InvalidArgument);
    auto staging = opened.value()->create_staging(42);
    ASSERT_TRUE(staging.ok());
    EXPECT_EQ(staging.value(), root_ / ".rodaq/artifacts/42.tmp");
    EXPECT_TRUE(fs::is_empty(staging.value()));
    std::ofstream(staging.value() / "canary") << "keep";
    EXPECT_EQ(opened.value()->create_staging(42).status().code(), StatusCode::Conflict);
    EXPECT_TRUE(fs::exists(staging.value() / "canary"));
    EXPECT_FALSE(fs::exists(root_ / ".rodaq/artifacts/42"));
}

TEST_F(ManagedVolumeTest, ExistingFactsWithoutMarkerAndSymlinkMetadataCannotBeInitialized) {
    ASSERT_TRUE(fs::create_directory(root_ / ".rodaq"));
    ASSERT_TRUE(fs::create_directory(root_ / ".rodaq/artifacts"));
    EXPECT_EQ(
        ManagedVolume::open(root_, VolumeOpenMode::Initialize).status().code(),
        StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(root_ / ".rodaq/volume.json"));
    fs::remove_all(root_ / ".rodaq");
    ASSERT_TRUE(fs::create_directory(parent_ / "foreign"));
    fs::create_directory_symlink(parent_ / "foreign", root_ / ".rodaq");
    EXPECT_FALSE(ManagedVolume::open(root_, VolumeOpenMode::Initialize).ok());
    EXPECT_TRUE(fs::is_empty(parent_ / "foreign"));
}

TEST_F(ManagedVolumeTest, UncertainMarkerPublishCanReconcileSameIdentityWithoutReinitializing) {
    unsigned directory_syncs = 0;
    auto failed =
        ManagedVolume::open(root_, VolumeOpenMode::Initialize, {}, [&](persistence::FileStep step) {
            if (step == persistence::FileStep::SyncDirectory && ++directory_syncs == 3) {
                return EIO;
            }
            return 0;
        });
    EXPECT_FALSE(failed.ok());
    const auto bytes = marker();
    ASSERT_FALSE(bytes.empty());
    auto recovered = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(marker(), bytes);
    EXPECT_TRUE(recovered.value()->verify().ok());
}

TEST_F(ManagedVolumeTest, ChangedMarkerContentsAndLostLeaseIdentityProtectTheActualOwner) {
    auto opened = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok());
    auto document = persistence::parse_json(marker());
    ASSERT_TRUE(document.ok());
    document.value().erase("content_digest");
    document.value()["created_at"] = "2026-01-01T00:00:00.000000000Z";
    auto altered = persistence::encode_integrity(document.value(), "rodaq.volume.v1");
    ASSERT_TRUE(altered.ok());
    auto published = persistence::publish_file(
        root_ / ".rodaq", "volume.json", altered.value(), persistence::FilePublishMode::Replace,
        4096);
    ASSERT_TRUE(published.status.ok());
    EXPECT_EQ(opened.value()->verify().status().code(), StatusCode::DataLoss);
    const auto identity = opened.value()->identity();
    opened.value().reset();
    auto readmitted = ManagedVolume::open(root_, VolumeOpenMode::Existing, identity);
    ASSERT_TRUE(readmitted.ok());
    ASSERT_TRUE(fs::remove(root_ / ".rodaq/.volume.lock"));
    EXPECT_EQ(readmitted.value()->verify().status().code(), StatusCode::DataLoss);
}

TEST_F(ManagedVolumeTest, ConcurrentPipelineStagingAndSpaceQueriesRetainTheSameVolumeIdentity) {
    auto opened = ManagedVolume::open(root_, VolumeOpenMode::Initialize);
    ASSERT_TRUE(opened.ok());
    const auto owner = opened.value();
    std::vector<std::future<bool>> workers;
    for (std::uint64_t id = 1; id <= 8; ++id) {
        workers.push_back(std::async(std::launch::async, [owner, id] {
            return owner->create_staging(id).ok() && owner->available_bytes().ok() &&
                   owner->verify().ok();
        }));
    }
    for (auto& worker : workers) {
        EXPECT_TRUE(worker.get());
    }
    EXPECT_EQ(owner->protection(), StatusCode::Ok);
    EXPECT_EQ(
        std::distance(
            fs::directory_iterator(owner->artifacts_directory()), fs::directory_iterator()),
        8);
}

}  // namespace
