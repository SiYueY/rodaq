#include "pipeline/pipeline_controller.hpp"
#include "artifact/segment_committer.hpp"
#include "task/recovery_store.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <thread>

namespace {
namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;
namespace plugin = rodaq::plugin;
namespace fs = std::filesystem;
using rodaq::StatusCode;
using namespace std::chrono_literals;

rodaq::Result<rodaq::time::ClockSample> receive_clock() {
    return rodaq::time::ClockSample{200, 1100, 1};
}

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

class PipelineControllerTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-pipeline-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        fs::create_directory(root_ / "packages");
        fs::copy(
            RODAQ_PIPELINE_STORAGE_PACKAGE, root_ / "packages/storage",
            fs::copy_options::recursive);
        fs::copy(
            RODAQ_PIPELINE_ACQUISITION_PACKAGE, root_ / "packages/acquisition",
            fs::copy_options::recursive);
        for (const auto& entry : fs::recursive_directory_iterator(root_ / "packages")) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root_ / "packages", fs::perms::owner_read | fs::perms::owner_exec);
        plugin::PluginManagerOptions plugins;
        plugins.search_paths = {root_ / "packages"};
        plugins.max_loaded_plugins = 2;
        plugins.max_instances = 4;
        plugins.max_config_size = 4096;
        plugins.shutdown_timeout = 2s;
        auto manager = plugin::PluginManager::create(std::move(plugins));
        ASSERT_TRUE(manager.ok());
        manager_ = std::make_shared<plugin::PluginManager>(std::move(manager.value()));
        for (const auto* name : {"time", "volume", "ledger", "recovery"}) {
            fs::create_directory(root_ / name);
        }
        auto boot =
            rodaq::time::parse_boot_identity("11234567-89ab-4cde-8fab-0123456789ab").value();
        auto clock = rodaq::time::ClockCorrelation::create(
            root_ / "time", boot, {100, 1000, 1}, {rodaq::time::BootArtifactState::Absent, {}});
        ASSERT_TRUE(clock.ok());
        clock_ = std::move(clock.value());
        auto volume =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(volume.ok());
        volume_ = std::move(volume.value());
        auto ledger =
            artifact::LedgerStore::open(root_ / "ledger", artifact::LedgerOpenMode::Initialize, 8);
        ASSERT_TRUE(ledger.ok());
        ledger_ = std::shared_ptr<artifact::LedgerStore>(std::move(ledger.value()));
        capacity_ = std::make_shared<artifact::VolumeCapacity>(std::move(
            artifact::VolumeCapacity::create({1000000, 1000000, 100, 10000, 8, 8, 16}).value()));
        auto probe = capacity_->begin_space_sample(100).value();
        ASSERT_TRUE(capacity_->finish_space_sample(probe, 1000000, 100).ok());
        auto lane = rodaq::ExecutionLane::create(1, 8);
        ASSERT_TRUE(lane.ok());
        lane_ = std::move(lane.value());
        options_.manifest = rodaq::test::manifest_identity();
        options_.identity = {
            options_.manifest.task_id, options_.manifest.pipeline_id,
            pipeline::parse_instance_id(options_.manifest.instance_id).value(), 7};
        options_.definition.pipeline_id = options_.identity.pipeline_id;
        options_.definition.acquisition = {
            "rodaq.fake.acquisition", "1.0.0",
            R"({"streams":["first","second"],"period_us":1000})"};
        options_.definition.storage = {"rodaq.fake.storage", "1.0.0", "{}"};
        options_.definition.streams = {
            {1, "first", {}, rodaq::ReadinessPolicy::EntryReady},
            {2, "second", {}, rodaq::ReadinessPolicy::EntryReady}};
        options_.streams = {
            {1, "first", 8, {1, 1001}, {1, 1001}}, {2, "second", 8, {1, 1001}, {1, 1001}}};
        options_.limits.records = 32;
        options_.limits.logical_bytes = 256;
        options_.limits.payloads = {16384, 256, 8, 32};
        options_.limits.queue_capacity = 16;
        options_.limits.callback_concurrency = 2;
        options_.envelope = {
            {sizeof(rodaq::storage::PrepareConfig)},
            8,
            24,
            3,
            8,
            100,
            800,
            100000,
            1,
            32,
            clock_->anchor().boot_id,
            100,
            1000};
        options_.manifest_limits.max_bytes = 65536;
        options_.max_prepared_descriptors = 16;
        options_.plugins = manager_;
        options_.writer_lane = lane_;
        options_.clock = clock_;
        options_.volume = volume_;
        options_.capacity = capacity_;
        options_.ledger = ledger_;
        options_.receive_clock = &receive_clock;
    }

    void TearDown() override {
        if (controller_) {
            (void)controller_->establish_control_fence();
            (void)controller_->close_admission(8);
            auto drained = controller_->drain(soon());
            if (drained.ok() && drained.value()) {
                commit(std::move(*drained.value()));
            }
            auto quiet = controller_->cleanup(soon());
            if (!quiet.ok()) {
                auto state = controller_->snapshot();
                if (state.ok() && state.value().quiescence.storage_closed &&
                    state.value().artifact_work_id != 0) {
                    std::error_code error;
                    fs::remove_all(volume_->artifacts_directory() / "7.tmp", error);
                    EXPECT_FALSE(error);
                    EXPECT_TRUE(
                        rodaq::persistence::sync_directory(volume_->artifacts_directory()).ok());
                    EXPECT_TRUE(
                        controller_->acknowledge_deleted(state.value().artifact_work_id, 7).ok());
                    quiet = controller_->cleanup(soon());
                }
            }
            if (!quiet.ok()) {
                EXPECT_TRUE(manager_->shutdown(soon()).ok());
                quiet = controller_->cleanup(soon());
            }
            EXPECT_TRUE(quiet.ok());
            EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
        }
        EXPECT_TRUE(lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
        controller_.reset();
        EXPECT_TRUE(manager_->shutdown(soon()).ok());
        manager_.reset();
        std::error_code error;
        fs::permissions(root_ / "packages", fs::perms::owner_all, error);
        for (const auto& entry : fs::recursive_directory_iterator(root_ / "packages", error)) {
            fs::permissions(entry.path(), fs::perms::owner_all, error);
        }
        fs::remove_all(root_, error);
    }

    void create() {
        auto controller = pipeline::PipelineController::create(options_);
        ASSERT_TRUE(controller.ok());
        controller_ = std::move(controller.value());
    }

    pipeline::OpenAuthorization authorize(const pipeline::PreparedPipeline& prepared) {
        // Real Recovery publication, at the Pipeline component boundary. This
        // is not the not-yet-integrated TaskService Start acceptance test.
        auto recovery =
            rodaq::task::TaskRecoveryStore::open(root_ / "recovery", options_.identity.task_id)
                .value();
        rodaq::task::TaskRecoveryRecord record;
        record.task_id = options_.identity.task_id;
        record.observed_control_generation = 7;
        record.active_config_version = options_.manifest.config_version;
        rodaq::task::RunRecovery run;
        run.operation_id.value[0] = 1;
        run.control_generation = 7;
        run.authorized_pipelines.push_back(
            {options_.identity.pipeline_id,
             options_.identity.instance_id,
             options_.manifest.effective_config_id,
             {"11234567-89ab-4cde-8fab-0123456789ab", prepared.prepared_monotonic},
             true,
             false,
             {}});
        record.run_operation = run;
        EXPECT_TRUE(recovery->publish(record, 0).ok());
        ++record.record_revision;
        record.run_operation->phase = rodaq::task::RunPhase::OpenAuthorized;
        record.run_operation->authorization_boundary =
            rodaq::task::RecoveryBoundary{"11234567-89ab-4cde-8fab-0123456789ab", 200};
        EXPECT_TRUE(recovery->publish(record, 1).ok());
        return {
            options_.identity.task_id,
            prepared.pipeline_id,
            prepared.instance_id,
            7,
            options_.manifest.effective_config_id,
            prepared.prepared_monotonic,
            prepared.boot_id,
            200,
            0};
    }

    bool wait_written(std::uint64_t count) {
        const auto end = std::chrono::steady_clock::now() + 2s;
        do {
            if (controller_->snapshot().value().records.written >= count) {
                return true;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }

    void commit(pipeline::SealedPipelineSegment sealed) {
        auto files = artifact::SealedFiles::open(
            volume_, 7, sealed.files, sealed.envelope, sealed.manifest_limits.max_bytes);
        ASSERT_TRUE(files.ok());
        auto manifest = sealed.segment->metadata.capture_manifest(files.value()->evidence());
        ASSERT_TRUE(manifest.ok());
        auto reservation = sealed.segment->capacity.handoff_sealed(files.value()->storage_bytes());
        ASSERT_TRUE(reservation.ok());
        auto committer = artifact::SegmentCommitter::create(
            std::move(files.value()), std::move(manifest.value()), sealed.manifest_limits,
            std::move(reservation.value()));
        ASSERT_TRUE(committer.ok());
        ASSERT_TRUE(committer.value()->commit(*ledger_).ok());
        // Component Artifact sink: publish verified Ledger facts into the
        // test's bounded projection, then actually transfer the retained charge.
        // Production ArtifactService/query/lease integration remains separate.
        published_.push_back(committer.value()->ledger_record());
        auto charge = committer.value()->take_committed_charge();
        ASSERT_TRUE(charge.ok());
        charges_.push_back(std::move(charge.value()));
        ASSERT_TRUE(controller_->acknowledge_artifact(sealed.work_id, 7).ok());
        commits_.push_back(std::move(committer.value()));
    }

    fs::path root_;
    pipeline::PipelineOptions options_;
    std::shared_ptr<plugin::PluginManager> manager_;
    std::shared_ptr<rodaq::ExecutionLane> lane_;
    std::shared_ptr<rodaq::time::ClockCorrelation> clock_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::shared_ptr<artifact::VolumeCapacity> capacity_;
    std::shared_ptr<artifact::LedgerStore> ledger_;
    std::shared_ptr<pipeline::PipelineController> controller_;
    std::vector<artifact::LedgerRecord> published_;
    std::vector<artifact::ArtifactCharge> charges_;
    std::vector<std::unique_ptr<artifact::SegmentCommitter>> commits_;
};

TEST_F(
    PipelineControllerTest,
    PrepareStartsRealSourceWhileClosedThenAutomaticWriterDrainsExactAcceptedSet) {
    create();
    ASSERT_TRUE(controller_);
    auto prepared = controller_->prepare(soon());
    ASSERT_TRUE(prepared.ok());
    EXPECT_FALSE(controller_->snapshot().value().gate.open);
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
    auto authorized = authorize(prepared.value());
    EXPECT_EQ(controller_->open_admission(authorized), pipeline::GateOpenResult::Opened);
    EXPECT_EQ(controller_->open_admission(authorized), pipeline::GateOpenResult::Opened);
    ASSERT_TRUE(wait_written(5));
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    auto closed = controller_->close_admission(8);
    ASSERT_TRUE(closed.ok());
    EXPECT_EQ(controller_->open_admission(authorized), pipeline::GateOpenResult::Obsolete);
    auto drained = controller_->drain(soon());
    ASSERT_TRUE(drained.ok());
    ASSERT_TRUE(drained.value());
    auto state = controller_->snapshot().value();
    EXPECT_EQ(state.records.accepted, closed.value().accepted_records);
    EXPECT_EQ(state.records.written, state.records.accepted);
    EXPECT_EQ(state.records.known_failed, 0);
    EXPECT_EQ(state.quiescence.buffered_records, 0);
    EXPECT_FALSE(state.quiescence.settled);
    EXPECT_EQ(state.quiescence.commit_inflight, 1);
    EXPECT_FALSE(controller_->quiesce(soon()).ok());
    commit(std::move(*drained.value()));
    ASSERT_TRUE(controller_->quiesce(soon()).ok());
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / "7/manifest.json"));
    auto repeated = controller_->drain(soon());
    ASSERT_TRUE(repeated.ok());
    EXPECT_FALSE(repeated.value());
}

TEST_F(
    PipelineControllerTest, EmptyPreparedInstanceClosesWithoutCreatingOrPublishingRecordingFiles) {
    create();
    ASSERT_TRUE(controller_);
    ASSERT_TRUE(controller_->prepare(soon()).ok());
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    auto drained = controller_->drain(soon());
    ASSERT_TRUE(drained.ok());
    EXPECT_FALSE(drained.value());
    ASSERT_TRUE(controller_->quiesce(soon()).ok());
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
    EXPECT_EQ(capacity_->snapshot().segment_slots, 0);
}

TEST_F(PipelineControllerTest, SourceReadyWaitsForActualVisibleSourceAndLateOpenCannotCrossFence) {
    options_.definition.streams[0].readiness_policy = rodaq::ReadinessPolicy::SourceReady;
    options_.definition.streams[1].readiness_policy = rodaq::ReadinessPolicy::SourceReady;
    create();
    ASSERT_TRUE(controller_);
    auto prepared = controller_->prepare(soon());
    ASSERT_TRUE(prepared.ok());
    EXPECT_EQ(controller_->callback_facts().value().streams[0].active_sources, 1);
    auto authorized = authorize(prepared.value());
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    EXPECT_EQ(controller_->open_admission(authorized), pipeline::GateOpenResult::Obsolete);
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
}

TEST_F(
    PipelineControllerTest,
    LateNativeWriteRetainsRecordsAndCannotBecomeQuiescentAtObservationDeadline) {
    options_.definition.storage.json = R"({"mode":"late_write","delay_ms":80})";
    create();
    ASSERT_TRUE(controller_);
    auto prepared = controller_->prepare(soon()).value();
    auto authorized = authorize(prepared);
    ASSERT_EQ(controller_->open_admission(authorized), pipeline::GateOpenResult::Opened);
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (controller_->snapshot().value().records.native_inflight == 0 &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::yield();
    }
    ASSERT_GT(controller_->snapshot().value().records.native_inflight, 0);
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    auto observed = controller_->drain(plugin::Deadline::after(1ms).value());
    EXPECT_FALSE(observed.ok());
    EXPECT_FALSE(controller_->snapshot().value().quiescence.settled);
    auto actual = controller_->drain(soon());
    ASSERT_TRUE(actual.ok());
    ASSERT_TRUE(actual.value());
    EXPECT_EQ(actual.value()->segment->metadata.snapshot().unknown, 0);
    commit(std::move(*actual.value()));
    ASSERT_TRUE(controller_->quiesce(soon()).ok());
}

TEST_F(PipelineControllerTest, FailedSourcePrepareClosesActualOwnersWithoutPublishingFiles) {
    options_.definition.acquisition.json =
        R"({"streams":["first","second"],"mode":"fail_prepare"})";
    create();
    ASSERT_TRUE(controller_);
    EXPECT_FALSE(controller_->prepare(soon()).ok());
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    ASSERT_TRUE(controller_->drain(soon()).ok());
    ASSERT_TRUE(controller_->cleanup(soon()).ok());
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
    EXPECT_EQ(capacity_->snapshot().segment_slots, 0);
}

TEST_F(PipelineControllerTest, UnknownTailRetainsStableObligationUntilActualDeletion) {
    options_.definition.storage.json = R"({"mode":"prefix_unknown"})";
    create();
    ASSERT_TRUE(controller_);
    auto prepared = controller_->prepare(soon());
    ASSERT_TRUE(prepared.ok());
    ASSERT_EQ(
        controller_->open_admission(authorize(prepared.value())), pipeline::GateOpenResult::Opened);
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (controller_->snapshot().value().records.unknown == 0 &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::yield();
    }
    ASSERT_GT(controller_->snapshot().value().records.unknown, 0);
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    auto drained = controller_->drain(soon());
    ASSERT_FALSE(drained.ok());
    EXPECT_EQ(drained.status().code(), StatusCode::DataLoss);
    const auto obligation = controller_->snapshot().value().artifact_work_id;
    ASSERT_NE(obligation, 0);
    EXPECT_FALSE(controller_->drain(soon()).ok());
    EXPECT_EQ(controller_->snapshot().value().artifact_work_id, obligation);
    EXPECT_FALSE(controller_->cleanup(soon()).ok());
    auto actual = controller_->snapshot().value();
    EXPECT_TRUE(actual.quiescence.storage_closed);
    EXPECT_TRUE(actual.quiescence.source_quiesced);
    EXPECT_FALSE(actual.quiescence.settled);
    EXPECT_FALSE(controller_->acknowledge_artifact(obligation, 7).ok());
    std::error_code error;
    EXPECT_GT(fs::remove_all(volume_->artifacts_directory() / "7.tmp", error), 0);
    ASSERT_FALSE(error);
    ASSERT_TRUE(rodaq::persistence::sync_directory(volume_->artifacts_directory()).ok());
    ASSERT_TRUE(controller_->acknowledge_deleted(obligation, 7).ok());
    ASSERT_TRUE(controller_->cleanup(soon()).ok());
    auto repeated = controller_->drain(soon());
    ASSERT_TRUE(repeated.ok());
    EXPECT_FALSE(repeated.value());
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
    EXPECT_EQ(capacity_->snapshot().segment_slots, 0);
}

}  // namespace
