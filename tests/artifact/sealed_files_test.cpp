#include "artifact/sealed_files.hpp"
#include "persistence/digest.hpp"
#include "persistence/file_descriptor.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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
        write("data.bin", "abc");
        write("metadata", "");
        seal_.files = {{"data.bin", 3}, {"metadata", 0}};
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

    std::string encoded_manifest() const {
        // File-owner test input, not evidence of full Manifest schema acceptance.
        return persistence::encode_integrity(
                   {{"format_version", "1.0"}, {"segment_id", "1"}}, "rodaq.segment-manifest.v1")
            .value();
    }

    fs::path root_;
    fs::path staging_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    rodaq::storage::SegmentSealResult seal_;
    rodaq::storage::PrepareConfig limits_{};
};

TEST_F(SealedFilesTest, ComputesActualFileDigestsAndSyncsPinnedFlatTreeWithoutClaimingCommitted) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto& owner = opened.value();
    ASSERT_EQ(owner->evidence().size(), 2);
    EXPECT_EQ(
        owner->evidence()[0].digest,
        "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(
        owner->evidence()[1].digest,
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(owner->storage_bytes(), 3);
    EXPECT_FALSE(owner->durable_tree());
    EXPECT_EQ(owner->sync_tree().status().code(), StatusCode::InvalidState);
    const auto manifest = encoded_manifest();
    ASSERT_TRUE(owner->install_manifest(manifest).ok());
    EXPECT_EQ(owner->manifest_bytes(), manifest.size());
    ASSERT_TRUE(owner->sync_tree().ok());
    EXPECT_TRUE(owner->durable_tree());
    EXPECT_TRUE(owner->verify().ok());
    EXPECT_TRUE(owner->verify().ok());
    EXPECT_TRUE(fs::exists(staging_));
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "1"));
    EXPECT_FALSE(owner->install_manifest(manifest).ok());
}

TEST_F(SealedFilesTest, StreamingDigestCrossesBufferBoundariesAndDoesNotChangeBorrowedFdOffset) {
    std::string data(200003, '\0');
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<char>(index % 251);
    }
    write("data.bin", data);
    persistence::FileDescriptor fd(::open((staging_ / "data.bin").c_str(), O_RDONLY | O_CLOEXEC));
    ASSERT_GE(fd.get(), 0);
    ASSERT_EQ(lseek(fd.get(), 7, SEEK_SET), 7);
    const auto expected = persistence::byte_digest(data.data(), data.size());
    ASSERT_TRUE(expected.ok());
    const auto actual = persistence::file_digest(fd.get(), data.size());
    ASSERT_TRUE(actual.ok());
    EXPECT_EQ(actual.value(), expected.value());
    EXPECT_EQ(lseek(fd.get(), 0, SEEK_CUR), 7);
    EXPECT_EQ(
        persistence::file_digest(fd.get(), data.size() + 1).status().code(), StatusCode::DataLoss);
}

TEST_F(SealedFilesTest, RejectsUnlistedEntriesDirectoriesLinksAndWrongSizesBeforeTreeProof) {
    write("unlisted", "extra");
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
    fs::remove(staging_ / "unlisted");
    fs::remove(staging_ / "metadata");
    ASSERT_TRUE(fs::create_directory(staging_ / "metadata"));
    EXPECT_EQ(open().status().code(), StatusCode::DataLoss);
    fs::remove(staging_ / "metadata");
    fs::create_symlink("data.bin", staging_ / "metadata");
    EXPECT_FALSE(open().ok());
    fs::remove(staging_ / "metadata");
    fs::create_hard_link(staging_ / "data.bin", staging_ / "metadata");
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

TEST_F(SealedFilesTest, RenamedOrModifiedPinnedFileCannotRetainItsOldDigestProof) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    fs::rename(staging_ / "data.bin", staging_ / "old-data");
    write("data.bin", "abc");
    EXPECT_EQ(opened.value()->verify().status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(opened.value()->install_manifest(encoded_manifest()).ok());
    fs::remove(staging_ / "data.bin");
    fs::rename(staging_ / "old-data", staging_ / "data.bin");
    auto fresh = open();
    ASSERT_TRUE(fresh.ok());
    write("data.bin", "modified");
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
