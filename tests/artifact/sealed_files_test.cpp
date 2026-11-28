#include "artifact/sealed_files.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace {

namespace artifact = rodaq::artifact;
namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using rodaq::StatusCode;

class SealedFilesTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-sealed-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        auto volume = artifact::ManagedVolume::open(root_, artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(volume.ok());
        volume_ = std::move(volume.value());
        auto staging = volume_->create_staging(1);
        ASSERT_TRUE(staging.ok());
        staging_ = staging.value();
        write("data.mcap", "abc");
        write("metadata", "");
        seal_.files = {{"data.mcap", 3}, {"metadata", 0}};
        seal_.total_physical_bytes = 3;
        limits_.max_segment_file_count = 4;
        limits_.max_relative_path_bytes = 128;
        limits_.max_segment_physical_bytes = 1024 * 1024;
    }

    void TearDown() override {
        volume_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void write(const char* name, const std::string& bytes) {
        std::ofstream file(staging_ / name, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        ASSERT_TRUE(file);
    }

    rodaq::Result<std::unique_ptr<artifact::SealedFiles>> open(
        persistence::FileFaultHook faults = {}) {
        return artifact::SealedFiles::open(volume_, 1, seal_, limits_, 4096, std::move(faults));
    }

    std::string encoded_manifest() const { return R"({"format_version":"1.0","segment_id":"1"})"; }

    fs::path root_;
    fs::path staging_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    rodaq::storage::SegmentSealResult seal_;
    rodaq::storage::PrepareConfig limits_{};
};

TEST_F(SealedFilesTest, VerifiesExactSizesAndSyncsPinnedFlatTreeWithoutClaimingCommitted) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto& owner = opened.value();
    ASSERT_EQ(owner->evidence().size(), 2U);
    EXPECT_EQ(owner->evidence()[0].relative_path, "data.mcap");
    EXPECT_EQ(owner->evidence()[0].physical_size, 3U);
    EXPECT_EQ(owner->evidence()[1].relative_path, "metadata");
    EXPECT_EQ(owner->evidence()[1].physical_size, 0U);
    EXPECT_EQ(owner->storage_bytes(), 3U);
    EXPECT_FALSE(owner->durable_tree());
    EXPECT_EQ(owner->sync_tree().status().code(), StatusCode::InvalidState);
    const auto manifest = encoded_manifest();
    ASSERT_TRUE(owner->install_manifest(manifest).ok());
    EXPECT_EQ(owner->manifest_bytes(), manifest.size());
    ASSERT_TRUE(owner->sync_tree().ok());
    EXPECT_TRUE(owner->durable_tree());
    EXPECT_TRUE(owner->verify().ok());
    EXPECT_TRUE(fs::exists(staging_));
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "1"));
    EXPECT_FALSE(owner->install_manifest(manifest).ok());
}

TEST_F(SealedFilesTest, ManifestInstallWithoutSyncSkipsFsyncCalls) {
    auto opened = open([](persistence::FileStep step) {
        return (step == persistence::FileStep::SyncFile ||
                step == persistence::FileStep::SyncDirectory)
                   ? EIO
                   : 0;
    });
    ASSERT_TRUE(opened.ok());
    const auto manifest = encoded_manifest();
    ASSERT_TRUE(opened.value()->install_manifest(manifest, false).ok());
    EXPECT_EQ(opened.value()->manifest_bytes(), manifest.size());
    EXPECT_TRUE(opened.value()->verify().ok());
}

TEST_F(SealedFilesTest, RejectsUnlistedEntriesDirectoriesLinksAndWrongSizesBeforeTreeProof) {
    write("unlisted", "extra");
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
    fs::remove(staging_ / "unlisted");
    fs::remove(staging_ / "metadata");
    ASSERT_TRUE(fs::create_directory(staging_ / "metadata"));
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
    fs::remove(staging_ / "metadata");
    fs::create_symlink("data.mcap", staging_ / "metadata");
    EXPECT_FALSE(open().ok());
    fs::remove(staging_ / "metadata");
    fs::create_hard_link(staging_ / "data.mcap", staging_ / "metadata");
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
    fs::remove(staging_ / "metadata");
    write("metadata", "wrong size");
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
}

TEST_F(SealedFilesTest, FifoCannotBlockACommitWorkerOrBecomeRegularFileEvidence) {
    fs::remove(staging_ / "metadata");
    ASSERT_EQ(mkfifo((staging_ / "metadata").c_str(), 0600), 0);
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
}

TEST_F(SealedFilesTest, RenamedOrResizedPinnedFileFailsVerification) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    fs::rename(staging_ / "data.mcap", staging_ / "old-data");
    write("data.mcap", "abc");
    EXPECT_EQ(opened.value()->verify().status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(opened.value()->install_manifest(encoded_manifest()).ok());
    fs::remove(staging_ / "data.mcap");
    fs::rename(staging_ / "old-data", staging_ / "data.mcap");
    auto fresh = open();
    ASSERT_TRUE(fresh.ok());
    write("data.mcap", "modified");
    EXPECT_EQ(fresh.value()->verify().status().code(), StatusCode::DataLoss);
}

TEST_F(SealedFilesTest, ManifestReserveAndBackendReservedNameAreEnforced) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ(
        opened.value()->install_manifest(std::string(4097, 'x')).status().code(),
        StatusCode::InvalidArgument);
    EXPECT_FALSE(fs::exists(staging_ / "manifest.json"));
    seal_.files[1].relative_path = "manifest.json";
    EXPECT_EQ(open().status().code(), StatusCode::InvalidArgument);
    seal_.files[1].relative_path = "../metadata";
    EXPECT_EQ(open().status().code(), StatusCode::InvalidArgument);
}

TEST_F(SealedFilesTest, ActualFileAndDirectorySyncFailuresNeverProduceDurableTree) {
    bool armed = false;
    auto failure_step = persistence::FileStep::SyncFile;
    auto opened =
        open([&](persistence::FileStep step) { return armed && step == failure_step ? EIO : 0; });
    ASSERT_TRUE(opened.ok());
    ASSERT_TRUE(opened.value()->install_manifest(encoded_manifest()).ok());
    armed = true;
    EXPECT_FALSE(opened.value()->sync_tree().ok());
    EXPECT_FALSE(opened.value()->durable_tree());
    armed = false;
    ASSERT_TRUE(opened.value()->sync_tree().ok());
    EXPECT_TRUE(opened.value()->durable_tree());
    failure_step = persistence::FileStep::SyncDirectory;
    armed = true;
    EXPECT_FALSE(opened.value()->sync_tree().ok());
    EXPECT_FALSE(opened.value()->durable_tree());
    armed = false;
    EXPECT_TRUE(opened.value()->sync_tree().ok());
}

TEST_F(SealedFilesTest, UncertainManifestPublicationNeverRetriesOrPretendsTreeDurability) {
    auto opened = open([](persistence::FileStep step) {
        return step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    const auto manifest = encoded_manifest();
    EXPECT_FALSE(opened.value()->install_manifest(manifest).ok());
    EXPECT_TRUE(fs::exists(staging_ / "manifest.json"));
    EXPECT_FALSE(opened.value()->install_manifest(manifest).ok());
    EXPECT_FALSE(opened.value()->sync_tree().ok());
    EXPECT_FALSE(opened.value()->durable_tree());
}

}  // namespace
