#include "pipeline/pipeline_controller.hpp"
#include "artifact/artifact_catalog.hpp"
#include "artifact/startup_scan.hpp"
#include "persistence/atomic_file.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace pipeline = rodaq::pipeline;
namespace artifact = rodaq::artifact;
namespace plugin = rodaq::plugin;
namespace fs = std::filesystem;
using rodaq::StatusCode;
using namespace std::chrono_literals;

std::atomic<std::uint64_t> mono_clock{1000};

rodaq::Result<rodaq::time::ClockSample> test_clock() {
    const auto now = mono_clock.fetch_add(1000000);
    return rodaq::time::ClockSample{now, static_cast<std::int64_t>(1700000000000000000LL + now), 1};
}

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

class PipelineControllerTest : public testing::Test {
protected:
    void SetUp() override {
        mono_clock = 1000;
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
        plugin::PluginManagerOptions plugins;
        plugins.search_paths = {root_ / "packages"};
        plugins.max_loaded_plugins = 2;
        plugins.max_instances = 8;
        plugins.max_config_size = 4096;
        plugins.shutdown_timeout = 2s;
        auto manager = plugin::PluginManager::create(std::move(plugins));
        ASSERT_TRUE(manager.ok());
        manager_ = std::make_shared<plugin::PluginManager>(std::move(manager.value()));

        fs::create_directory(root_ / "volume");
        auto volume =
            artifact::ManagedVolume::open(root_ / "volume", artifact::VolumeOpenMode::Initialize);
        ASSERT_TRUE(volume.ok());
        volume_ = std::move(volume.value());

        capacity_ = std::make_shared<artifact::VolumeCapacity>(
            artifact::VolumeCapacity::create(
                {10000000, 4000000, 100, 100000000000ULL, 16, 16, 32, 200, 400, 1000})
                .value());
        auto probe = capacity_->begin_space_sample(100).value();
        ASSERT_TRUE(capacity_->finish_space_sample(probe, 10000000, 100).ok());

        artifact::CatalogOptions cat_opts;
        cat_opts.volume = volume_;
        cat_opts.capacity = capacity_;
        cat_opts.max_artifacts = 32;
        auto catalog = artifact::ArtifactCatalog::create(cat_opts);
        ASSERT_TRUE(catalog.ok());
        catalog_ = std::move(catalog.value());
        ASSERT_TRUE(catalog_->startup_scan().ok());

        auto lane = rodaq::ExecutionLane::create(1, 16);
        ASSERT_TRUE(lane.ok());
        lane_ = std::move(lane.value());
    }

    void TearDown() override {
        if (lane_) {
            (void)lane_->shutdown(std::chrono::steady_clock::now() + 2s);
        }
        lane_.reset();
        catalog_.reset();
        if (manager_) {
            (void)manager_->shutdown(soon());
        }
        manager_.reset();
        volume_.reset();
        capacity_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    pipeline::PipelineOptions make_options(
        std::uint64_t segment_id, rodaq::PipelineId pipeline_id, std::string storage_config = "{}",
        std::string acq_config =
            R"({"streams":["first","second"],"period_us":1000,"payload_bytes":4})") {
        pipeline::PipelineOptions opts;
        opts.manifest = rodaq::test::manifest_identity(segment_id);
        opts.manifest.pipeline_id = pipeline_id;
        opts.manifest.volume_id = volume_->identity();
        opts.identity = {
            opts.manifest.task_id, pipeline_id,
            pipeline::parse_instance_id(opts.manifest.instance_id).value(), 7};
        opts.definition.pipeline_id = pipeline_id;
        opts.definition.required = true;
        opts.definition.resource_profile = "default";
        opts.definition.output_root = "default";
        opts.definition.acquisition = {"rodaq.fake.acquisition", "1.0.0", std::move(acq_config)};
        opts.definition.storage = {"rodaq.fake.storage", "1.0.0", std::move(storage_config)};
        opts.definition.streams = {
            {1, "first", {}, rodaq::ReadinessPolicy::EntryReady},
            {2, "second", {}, rodaq::ReadinessPolicy::EntryReady}};
        opts.streams = {{1, "first", 16}, {2, "second", 16}};
        opts.limits.records = 64;
        opts.limits.logical_bytes = 1024;
        opts.limits.payloads = {32768, 256, 16, 64};
        opts.limits.queue_capacity = 32;
        opts.limits.callback_concurrency = 2;
        opts.envelope = {
            {sizeof(rodaq::storage::PrepareConfig)}, 16, 128, 8, 8, 1000, 16384, 1000000, 2, 64};
        opts.segment_reservation_bytes = 65536;
        opts.manifest_limits.max_bytes = 65536;
        opts.max_prepared_descriptors = 16;
        opts.plugins = manager_;
        opts.writer_lane = lane_;
        opts.volume = volume_;
        opts.capacity = capacity_;
        opts.receive_clock = &test_clock;
        return opts;
    }

    fs::path root_;
    std::shared_ptr<plugin::PluginManager> manager_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::shared_ptr<artifact::VolumeCapacity> capacity_;
    std::shared_ptr<artifact::ArtifactCatalog> catalog_;
    std::shared_ptr<rodaq::ExecutionLane> lane_;
};

TEST_F(PipelineControllerTest, FullPrepareOpenRotateDrainCommitAndCleanupLifecycle) {
    auto created = pipeline::PipelineController::create(make_options(7, 1));
    ASSERT_TRUE(created.ok()) << created.status().message();
    auto controller = std::move(created.value());

    auto prepared = controller->prepare(soon());
    ASSERT_TRUE(prepared.ok()) << prepared.status().message();
    EXPECT_EQ(prepared.value().pipeline_id, 1ULL);

    pipeline::OpenAuthorization auth;
    auth.task_id = rodaq::test::manifest_identity().task_id;
    auth.pipeline_id = 1;
    auth.instance_id = prepared.value().instance_id;
    auth.control_generation = 7;
    auth.prepared_monotonic = prepared.value().prepared_monotonic;
    auth.opened_monotonic = prepared.value().prepared_monotonic;
    auth.recording_base = 0;
    ASSERT_EQ(controller->open_admission(auth), pipeline::GateOpenResult::Opened);

    // Wait for records to be accepted, then rotate to segment 8.
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (controller->snapshot().value().gate.accepted_records < 2 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_GE(controller->snapshot().value().gate.accepted_records, 2ULL);

    auto rotated = controller->rotate(8, soon());
    ASSERT_TRUE(rotated.ok()) << rotated.status().message();
    ASSERT_TRUE(rotated.value().has_value());
    EXPECT_EQ(rotated.value()->segment->capacity_snapshot().segment_id, 7ULL);
    ASSERT_TRUE(catalog_->commit(*rotated.value()).ok());
    ASSERT_TRUE(controller->acknowledge_artifact(rotated.value()->work_id, 7).ok());

    // Wait for more records on segment 8, then close and drain.
    const auto until2 = std::chrono::steady_clock::now() + 2s;
    while (controller->snapshot().value().records.accepted < 2 &&
           std::chrono::steady_clock::now() < until2) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(controller->close_admission(7).ok());
    auto drained = controller->drain(soon());
    ASSERT_TRUE(drained.ok()) << drained.status().message();
    ASSERT_TRUE(drained.value().has_value());
    EXPECT_EQ(drained.value()->segment->capacity_snapshot().segment_id, 8ULL);
    ASSERT_TRUE(catalog_->commit(*drained.value()).ok());
    ASSERT_TRUE(controller->acknowledge_artifact(drained.value()->work_id, 8).ok());

    ASSERT_TRUE(controller->quiesce(soon()).ok());
    ASSERT_TRUE(controller->cleanup(soon()).ok());
    EXPECT_TRUE(controller->snapshot().value().quiescence.settled);
    EXPECT_EQ(catalog_->snapshot().visible, 2U);
    EXPECT_TRUE(catalog_->get(rodaq::ArtifactId{7}).ok());
    EXPECT_TRUE(catalog_->get(rodaq::ArtifactId{8}).ok());
}

TEST_F(PipelineControllerTest, StorageUnknownFailurePreventsNormalArtifactCommit) {
    auto created = pipeline::PipelineController::create(
        make_options(11, 1, R"({"mode":"prefix_unknown","prefix":1})"));
    ASSERT_TRUE(created.ok());
    auto controller = std::move(created.value());
    auto prepared = controller->prepare(soon());
    ASSERT_TRUE(prepared.ok());

    pipeline::OpenAuthorization auth;
    auth.task_id = rodaq::test::manifest_identity().task_id;
    auth.pipeline_id = 1;
    auth.instance_id = prepared.value().instance_id;
    auth.control_generation = 7;
    auth.prepared_monotonic = prepared.value().prepared_monotonic;
    auth.opened_monotonic = prepared.value().prepared_monotonic;
    ASSERT_EQ(controller->open_admission(auth), pipeline::GateOpenResult::Opened);

    const auto until = std::chrono::steady_clock::now() + 2s;
    while (controller->snapshot().value().gate.accepted_records < 3 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(controller->close_admission(7).ok());
    auto drained = controller->drain(soon());
    // Storage Unknown failure refuses normal commit and returns DataLoss.
    EXPECT_EQ(drained.status().code(), StatusCode::DataLoss);
    EXPECT_EQ(catalog_->snapshot().visible, 0U);
    const auto work_id = controller->snapshot().value().artifact_work_id;
    ASSERT_NE(work_id, 0ULL);
    // First cleanup closes acquisition + storage; artifact_work keeps it unsettled until staging
    // is removed and acknowledged.
    EXPECT_EQ(controller->cleanup(soon()).status().code(), StatusCode::InvalidState);
    std::error_code ec;
    fs::remove_all(volume_->artifacts_directory() / "11.tmp", ec);
    ASSERT_TRUE(controller->acknowledge_deleted(work_id, 11).ok());
    ASSERT_TRUE(controller->cleanup(soon()).ok());
    EXPECT_TRUE(controller->snapshot().value().quiescence.settled);
}

}  // namespace
