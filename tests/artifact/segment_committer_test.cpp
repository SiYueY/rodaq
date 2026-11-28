#include "artifact/artifact_catalog.hpp"
#include "artifact/segment_committer.hpp"
#include "artifact/startup_scan.hpp"
#include "daq/composition.hpp"
#include "execution_lane.hpp"
#include "manifest_fixture.hpp"
#include "persistence/atomic_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <optional>

namespace {

namespace artifact = rodaq::artifact;
namespace persistence = rodaq::persistence;
namespace fs = std::filesystem;
using rodaq::StatusCode;
using namespace std::chrono_literals;

class SegmentCommitterTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-commit-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        auto accounting = artifact::VolumeCapacity::create({1000000, 1000000, 0, 100, 16, 16, 32});
        ASSERT_TRUE(accounting.ok());
        volume_capacity_ =
            std::make_shared<artifact::VolumeCapacity>(std::move(accounting.value()));
        auto probe = volume_capacity_->begin_space_sample(100);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(volume_capacity_->finish_space_sample(probe.value(), 1000000, 100).ok());
        fs::create_directory(root_ / "volume");
        auto volume =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(volume.ok());
        volume_ = std::move(volume.value());
        auto staging = volume_->create_staging(7);
        ASSERT_TRUE(staging.ok());
        staging_ = staging.value();
        std::ofstream(staging_ / "data.mcap", std::ios::binary) << "abc";
        seal_.files = {{"data.mcap", 3}};
        seal_.total_physical_bytes = 3;
        envelope_.max_segment_file_count = 1;
        envelope_.max_relative_path_bytes = 32;
        envelope_.max_segment_physical_bytes = 1024;
        manifest_.identity = rodaq::test::manifest_identity(7);
        manifest_.identity.volume_id = volume_->identity();
        auto descriptor = rodaq::test::manifest_descriptor();
        descriptor.accepted_records = 2;
        manifest_.descriptors = {descriptor};
        manifest_.coverage = {{1, 2, 1, 1, 0, 10, 11}};
        manifest_.first_receive_monotonic = 100;
        manifest_.last_receive_monotonic = 200;
        manifest_.min_receive_realtime = 1100;
        manifest_.max_receive_realtime = 1200;
        manifest_.recording_begin_ns = 1090;
        manifest_.recording_end_ns = 1190;
    }

    void TearDown() override {
        volume_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    rodaq::Result<std::unique_ptr<artifact::SegmentCommitter>> prepare(
        persistence::FileFaultHook faults = {},
        rodaq::DurabilityProfile durability = rodaq::DurabilityProfile::Normal) {
        auto files =
            artifact::SealedFiles::open(volume_, 7, seal_, envelope_, 8192, std::move(faults));
        if (!files.ok()) {
            return files.status();
        }
        manifest_.files = files.value()->evidence();
        auto capacity = volume_capacity_->reserve(8195, 100);
        if (!capacity.ok()) {
            return capacity.status();
        }
        capacity.value().mark_effects_possible();
        auto usage = capacity.value().record_usage(3, 8192);
        if (!usage.ok()) {
            return usage.status();
        }
        artifact::CommitReservation reservation{std::move(capacity.value()), 3};
        return artifact::SegmentCommitter::create(
            files.value(), manifest_, {}, reservation, durability);
    }

    rodaq::Result<artifact::SealedSegment> handoff() {
        auto capacity = volume_capacity_->reserve(8195, 100);
        if (!capacity.ok()) {
            return capacity.status();
        }
        capacity.value().mark_effects_possible();
        manifest_.files = {{"data.mcap", 3}};
        return artifact::SealedSegment{
            1,         7,  manifest_.identity.task_id,      manifest_, seal_,
            envelope_, {}, {std::move(capacity.value()), 3}};
    }

    rodaq::Result<std::shared_ptr<artifact::ArtifactCatalog>> catalog() {
        artifact::CatalogOptions options;
        options.volume = volume_;
        options.capacity = volume_capacity_;
        options.max_artifacts = 32;
        return artifact::ArtifactCatalog::create(options);
    }

    fs::path final_dir() const { return volume_->artifacts_directory() / "7"; }

    fs::path root_;
    fs::path staging_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::shared_ptr<artifact::VolumeCapacity> volume_capacity_;
    rodaq::storage::SegmentSealResult seal_;
    rodaq::storage::PrepareConfig envelope_{};
    artifact::SegmentManifest manifest_;
};

TEST_F(SegmentCommitterTest, AtomicNoReplacePublicationSyncsTreeAndTransfersRetainedCharge) {
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok()) << prepared.status().message();
    auto& owner = prepared.value();
    EXPECT_FALSE(owner->committed());
    EXPECT_EQ(owner->take_committed_charge().status().code(), StatusCode::InvalidState);
    EXPECT_EQ(volume_capacity_->snapshot().commit_slots, 1U);
    ASSERT_TRUE(owner->commit().ok());
    EXPECT_TRUE(owner->committed());
    EXPECT_EQ(owner->storage_bytes(), 3U);
    EXPECT_FALSE(fs::exists(staging_));
    ASSERT_TRUE(fs::is_directory(final_dir()));

    auto bytes = persistence::read_file(final_dir(), "manifest.json", 8192);
    ASSERT_TRUE(bytes.ok());
    EXPECT_EQ(bytes.value().size(), owner->manifest_bytes());
    auto decoded = artifact::decode_manifest(bytes.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().identity.segment_id, 7ULL);

    EXPECT_EQ(owner->commit().status().code(), StatusCode::InvalidState);
    const auto retained_bytes = owner->storage_bytes() + owner->manifest_bytes();
    EXPECT_EQ(volume_capacity_->snapshot().actual_bytes, retained_bytes);

    auto retained = owner->take_committed_charge();
    ASSERT_TRUE(retained.ok());
    EXPECT_EQ(volume_capacity_->snapshot().commit_slots, 0U);
    EXPECT_EQ(volume_capacity_->snapshot().actual_bytes, retained_bytes);
    EXPECT_EQ(volume_capacity_->snapshot().artifact_slots, 1U);
    EXPECT_EQ(owner->take_committed_charge().status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(retained.value().release_after_delete().ok());
    EXPECT_EQ(volume_capacity_->snapshot().artifact_slots, 0U);
    EXPECT_EQ(volume_capacity_->snapshot().actual_bytes, 0U);
}

TEST_F(SegmentCommitterTest, NormalProfileCommitsAtomicallyWithoutMandatoryFsync) {
    auto prepared = prepare([](auto step) {
        return (step == persistence::FileStep::SyncFile ||
                step == persistence::FileStep::SyncDirectory)
                   ? EIO
                   : 0;
    });
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(prepared.value()->commit().ok());
    EXPECT_TRUE(prepared.value()->committed());
    EXPECT_FALSE(fs::exists(staging_));
    EXPECT_TRUE(fs::is_directory(final_dir()));
    EXPECT_TRUE(fs::is_regular_file(final_dir() / "manifest.json"));
}

TEST_F(SegmentCommitterTest, PowerLossHardenedTreeSyncFailureNeverPublishesFinalDirectory) {
    auto prepared = prepare(
        [](auto step) { return step == persistence::FileStep::SyncFile ? EIO : 0; },
        rodaq::DurabilityProfile::PowerLossHardened);
    ASSERT_TRUE(prepared.ok());
    EXPECT_FALSE(prepared.value()->commit().ok());
    EXPECT_FALSE(prepared.value()->committed());
    EXPECT_FALSE(fs::exists(final_dir()));
    EXPECT_TRUE(fs::exists(staging_));
}

TEST_F(SegmentCommitterTest, ExistingFinalDirectoryIsNeverOverwrittenOrAdopted) {
    fs::create_directory(final_dir());
    std::ofstream(final_dir() / "sentinel") << "keep";
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    EXPECT_EQ(prepared.value()->commit().status().code(), StatusCode::Conflict);
    EXPECT_FALSE(prepared.value()->committed());
    EXPECT_TRUE(fs::exists(final_dir() / "sentinel"));
    EXPECT_FALSE(fs::exists(final_dir() / "manifest.json"));
    EXPECT_TRUE(fs::exists(staging_ / "data.mcap"));
}

TEST_F(SegmentCommitterTest, UnknownOrEntirelyUnwrittenCoverageCannotEnterNormalCommit) {
    manifest_.coverage[0] = {1, 2, 1, 0, 1, 10, 11};
    EXPECT_EQ(prepare().status().code(), StatusCode::DataLoss);
    manifest_.coverage[0] = {1, 2, 0, 2, 0, 10, 11};
    EXPECT_EQ(prepare().status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(staging_ / "manifest.json"));
}

TEST_F(SegmentCommitterTest, FileResizeBeforeRenameIsDetectedAndBlocksPublication) {
    auto prepared = prepare([this, publishes = 0](auto step) mutable {
        if (step == persistence::FileStep::Publish && ++publishes == 2) {
            std::ofstream(staging_ / "data.mcap", std::ios::trunc) << "mutated-longer";
        }
        return 0;
    });
    ASSERT_TRUE(prepared.ok());
    EXPECT_EQ(prepared.value()->commit().status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(final_dir()));
}

// A closed or full Commit lane must return the original move-only owner.
struct BlockingWork final : rodaq::Work {
    std::promise<void> started;
    std::shared_future<void> released;
    explicit BlockingWork(std::shared_future<void> done) : released(std::move(done)) {}
    void run() noexcept override {
        started.set_value();
        released.wait();
    }
};

struct LaneGuard {
    std::shared_ptr<rodaq::ExecutionLane> lane;
    std::promise<void> release;
    bool released{false};
    explicit LaneGuard(std::shared_ptr<rodaq::ExecutionLane> owner) : lane(std::move(owner)) {}
    void unblock() {
        if (!released) {
            release.set_value();
            released = true;
        }
    }
    ~LaneGuard() {
        unblock();
        (void)lane->shutdown(std::chrono::steady_clock::now() + 2s);
    }
};

TEST_F(SegmentCommitterTest, ClosedCommitLaneReturnsOwnerAndAbortReleasesOnlyAfterCleanup) {
    auto cat = catalog();
    ASSERT_TRUE(cat.ok());
    auto lane = rodaq::ExecutionLane::create(1, 1);
    ASSERT_TRUE(lane.ok());
    ASSERT_TRUE(lane.value()->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    auto input = handoff();
    ASSERT_TRUE(input.ok());
    std::optional<artifact::SealedSegment> sealed(std::move(input.value()));
    bool ack = false;
    auto result =
        rodaq::daq::submit_commit(cat.value(), lane.value(), 10ms, sealed, [&] { ack = true; });
    EXPECT_EQ(result.status().code(), StatusCode::InvalidState);
    ASSERT_TRUE(sealed);
    EXPECT_FALSE(ack);
    EXPECT_TRUE(fs::exists(staging_));
    EXPECT_EQ(volume_capacity_->snapshot().commit_bytes, 8195U);
    EXPECT_EQ(volume_capacity_->snapshot().actual_bytes, 0U);
    ASSERT_TRUE(cat.value()->abort(*sealed).ok());
    sealed.reset();
    EXPECT_FALSE(fs::exists(staging_));
    EXPECT_EQ(volume_capacity_->snapshot().commit_bytes, 0U);
    EXPECT_EQ(volume_capacity_->snapshot().orphaned_reservations, 0U);
}

TEST_F(SegmentCommitterTest, FullCommitLaneReturnsOwnerAndAdmittedTimeoutRetainsIt) {
    auto cat = catalog();
    ASSERT_TRUE(cat.ok());
    auto lane = rodaq::ExecutionLane::create(1, 1);
    ASSERT_TRUE(lane.ok());
    LaneGuard guard{lane.value()};
    auto released = guard.release.get_future().share();
    auto blocking = std::make_shared<BlockingWork>(released);
    auto started = blocking->started.get_future();
    ASSERT_TRUE(guard.lane->submit(blocking).ok());
    ASSERT_EQ(started.wait_for(2s), std::future_status::ready);
    auto queued = std::make_shared<BlockingWork>(released);
    auto queued_started = queued->started.get_future();
    ASSERT_TRUE(guard.lane->submit(queued).ok());
    auto input = handoff();
    ASSERT_TRUE(input.ok());
    std::optional<artifact::SealedSegment> sealed(std::move(input.value()));
    auto result = rodaq::daq::submit_commit(cat.value(), guard.lane, 10ms, sealed, {});
    EXPECT_EQ(result.status().code(), StatusCode::ResourceExhausted);
    ASSERT_TRUE(sealed);
    EXPECT_EQ(volume_capacity_->snapshot().commit_bytes, 8195U);
    guard.unblock();
    ASSERT_EQ(queued_started.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(guard.lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());

    // Hold the worker with a fresh promise while the Commit is actually queued.
    std::promise<void> unblock;
    auto held = std::make_shared<BlockingWork>(unblock.get_future().share());
    auto held_started = held->started.get_future();
    ASSERT_TRUE(guard.lane->submit(held).ok());
    // Always release the worker before an assertion can leave this scope.
    const auto ready = held_started.wait_for(2s);
    auto acknowledged = std::make_shared<std::atomic<bool>>(false);
    auto timed = rodaq::daq::submit_commit(
        cat.value(), guard.lane, 1ms, sealed, [acknowledged] { *acknowledged = true; });
    const auto pending = volume_capacity_->snapshot();
    const bool staging_exists = fs::exists(staging_);
    const bool early_ack = acknowledged->load();
    unblock.set_value();
    ASSERT_TRUE(guard.lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_EQ(timed.status().code(), StatusCode::DeadlineExceeded);
    EXPECT_FALSE(sealed);
    EXPECT_TRUE(staging_exists);
    EXPECT_FALSE(early_ack);
    EXPECT_EQ(pending.commit_slots, 1U);
    EXPECT_TRUE(acknowledged->load());
    EXPECT_EQ(volume_capacity_->snapshot().orphaned_reservations, 0U);
    EXPECT_EQ(volume_capacity_->snapshot().commit_slots, 0U);
    EXPECT_TRUE(fs::is_directory(final_dir()));
}

TEST_F(SegmentCommitterTest, FailedCleanupRetainsChargeAndNeverDeletesPublishedFiles) {
    auto cat = catalog();
    ASSERT_TRUE(cat.ok());
    auto input = handoff();
    ASSERT_TRUE(input.ok());
    fs::create_directory(final_dir());
    std::ofstream(final_dir() / "sentinel") << "keep";
    bool settled = true;
    auto committed = cat.value()->commit(std::move(input.value()), &settled);
    EXPECT_FALSE(committed.ok());
    EXPECT_FALSE(settled);
    EXPECT_TRUE(fs::exists(final_dir() / "sentinel"));
    EXPECT_TRUE(fs::exists(staging_));
    const auto facts = volume_capacity_->snapshot();
    // The failed publication has already written the manifest and converted
    // the reservation into actual usage. Neither the files nor their charge
    // may disappear just because cleanup could not be proven.
    const auto retained_bytes =
        fs::file_size(staging_ / "data.mcap") + fs::file_size(staging_ / "manifest.json");
    EXPECT_EQ(facts.actual_bytes, retained_bytes);
    EXPECT_EQ(facts.commit_bytes, retained_bytes);
    EXPECT_EQ(facts.commit_slots, 1U);
    EXPECT_NE(facts.protection, StatusCode::Ok);
    EXPECT_FALSE(facts.actual_usage_complete);
}

TEST_F(SegmentCommitterTest, RejectedPhysicalEvidenceCleansStagingBeforeSettlement) {
    auto cat = catalog();
    ASSERT_TRUE(cat.ok());
    auto input = handoff();
    ASSERT_TRUE(input.ok());
    std::ofstream(staging_ / "data.mcap", std::ios::app) << "wrong-size";
    bool settled = false;
    auto result = cat.value()->commit(std::move(input.value()), &settled);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(settled);
    EXPECT_FALSE(fs::exists(staging_));
    EXPECT_FALSE(fs::exists(final_dir()));
    EXPECT_EQ(volume_capacity_->snapshot().commit_bytes, 0U);
    EXPECT_EQ(volume_capacity_->snapshot().orphaned_reservations, 0U);
}

TEST_F(SegmentCommitterTest, StartupScanCleansTmpAndDeletingSkipsCorruptAndRebuildsCatalog) {
    // Commit segment 7 cleanly.
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(prepared.value()->commit().ok());
    auto charge = prepared.value()->take_committed_charge();
    ASSERT_TRUE(charge.ok());

    // Create stale .tmp, stale .deleting, and a corrupt final artifact directory.
    fs::create_directory(volume_->artifacts_directory() / "8.tmp");
    std::ofstream(volume_->artifacts_directory() / "8.tmp/partial.mcap") << "tmp";
    fs::create_directory(volume_->artifacts_directory() / "9.deleting");
    std::ofstream(volume_->artifacts_directory() / "9.deleting/old.mcap") << "del";
    fs::create_directory(volume_->artifacts_directory() / "10");
    std::ofstream(volume_->artifacts_directory() / "10/manifest.json") << "{corrupt";

    auto fresh_cap = std::make_shared<artifact::VolumeCapacity>(
        artifact::VolumeCapacity::create({1000000, 1000000, 0, 100, 16, 16, 32}).value());
    artifact::CatalogOptions opts;
    opts.volume = volume_;
    opts.capacity = fresh_cap;
    opts.max_artifacts = 32;
    auto catalog = artifact::ArtifactCatalog::create(opts);
    ASSERT_TRUE(catalog.ok());
    auto scanned = catalog.value()->startup_scan();
    ASSERT_TRUE(scanned.ok()) << scanned.status().message();
    EXPECT_EQ(scanned.value().cleaned_tmp, 1U);
    EXPECT_EQ(scanned.value().cleaned_deleting, 1U);
    EXPECT_EQ(scanned.value().corrupt_count, 1U);
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "8.tmp"));
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "9.deleting"));
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / "10"));
    EXPECT_EQ(catalog.value()->snapshot().visible, 1U);
    EXPECT_EQ(catalog.value()->snapshot().corrupt_artifacts, 1U);
    const rodaq::ArtifactId id7{7};
    const rodaq::ArtifactId id10{10};
    EXPECT_TRUE(catalog.value()->get(id7).ok());
    EXPECT_EQ(catalog.value()->get(id10).status().code(), StatusCode::NotFound);

    // Rename-then-delete with active ReadLease waits/cancels and physically removes.
    auto lease = catalog.value()->acquire(id7, rodaq::ArtifactFileId{1});
    ASSERT_TRUE(lease.ok());
    auto deleting = std::async(std::launch::async, [&] {
        return catalog.value()->delete_artifact(
            id7, std::chrono::steady_clock::now() + std::chrono::seconds{2});
    });
    // Cancel readers must be noexcept and release waiting delete once handle is released.
    catalog.value()->cancel_readers(id7);
    lease = rodaq::Status(StatusCode::Unavailable, "released");
    ASSERT_TRUE(deleting.get().ok());
    EXPECT_EQ(catalog.value()->snapshot().visible, 0U);
    EXPECT_FALSE(fs::exists(final_dir()));
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7.deleting"));
    EXPECT_EQ(fresh_cap->snapshot().artifact_slots, 0U);
    EXPECT_EQ(fresh_cap->snapshot().actual_bytes, 0U);
}

TEST_F(SegmentCommitterTest, StartupScanExceedingMaxArtifactsFailsClosed) {
    auto prepared = prepare();
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(prepared.value()->commit().ok());

    auto fresh_cap = std::make_shared<artifact::VolumeCapacity>(
        artifact::VolumeCapacity::create({1000000, 1000000, 0, 100, 16, 16, 32}).value());
    artifact::CatalogOptions opts;
    opts.volume = volume_;
    opts.capacity = fresh_cap;
    opts.max_artifacts = 0;  // Invalid or exceeded
    EXPECT_FALSE(artifact::ArtifactCatalog::create(opts).ok());

    fs::create_directory(volume_->artifacts_directory() / "11");
    std::ofstream(volume_->artifacts_directory() / "11/manifest.json") << "{corrupt";
    opts.max_artifacts = 1;
    auto catalog = artifact::ArtifactCatalog::create(opts);
    ASSERT_TRUE(catalog.ok());
    EXPECT_EQ(catalog.value()->startup_scan().status().code(), StatusCode::ResourceExhausted);
}

TEST_F(SegmentCommitterTest, StartupScanFailsClosedOnLegacyManifest10BeforeCleaningTmpOrDeleting) {
    // Seed a legacy Manifest 1.0 artifact directory alongside .tmp and .deleting directories.
    const auto legacy_dir = volume_->artifacts_directory() / "12";
    fs::create_directory(legacy_dir);
    std::ofstream(legacy_dir / "data.mcap", std::ios::binary) << "old";
    std::ofstream(legacy_dir / "manifest.json", std::ios::binary)
        << R"({"format_version":"1.0","segment_id":"12","descriptors":[{"data_profile":2}]})";

    const auto tmp_dir = volume_->artifacts_directory() / "13.tmp";
    fs::create_directory(tmp_dir);
    std::ofstream(tmp_dir / "partial.mcap", std::ios::binary) << "tmp";

    const auto deleting_dir = volume_->artifacts_directory() / "14.deleting";
    fs::create_directory(deleting_dir);
    std::ofstream(deleting_dir / "old.mcap", std::ios::binary) << "del";

    auto fresh_cap = std::make_shared<artifact::VolumeCapacity>(
        artifact::VolumeCapacity::create({1000000, 1000000, 0, 100, 16, 16, 32}).value());
    artifact::CatalogOptions opts;
    opts.volume = volume_;
    opts.capacity = fresh_cap;
    opts.max_artifacts = 32;
    auto catalog = artifact::ArtifactCatalog::create(opts);
    ASSERT_TRUE(catalog.ok());

    const auto scanned = catalog.value()->startup_scan();
    ASSERT_FALSE(scanned.ok());
    EXPECT_EQ(scanned.status().code(), StatusCode::Unavailable);

    // Zero disk mutation: legacy 1.0 artifact, .tmp, and .deleting must remain untouched.
    EXPECT_TRUE(fs::is_regular_file(legacy_dir / "manifest.json"));
    EXPECT_TRUE(fs::is_regular_file(legacy_dir / "data.mcap"));
    EXPECT_TRUE(fs::is_regular_file(tmp_dir / "partial.mcap"));
    EXPECT_TRUE(fs::is_regular_file(deleting_dir / "old.mcap"));
}

}  // namespace
