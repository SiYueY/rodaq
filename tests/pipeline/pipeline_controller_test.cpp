#include "pipeline/pipeline_controller.hpp"
#include "artifact/segment_committer.hpp"
#include "task/recovery_store.hpp"
#include "task/run_authorization.hpp"
#include "task/task_controller.hpp"
#include "task/task_service.hpp"
#include "artifact/segment_id_store.hpp"
#include "persistence/json.hpp"
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
        for (const auto& lane : service_lanes_) {
            EXPECT_TRUE(lane->shutdown(std::chrono::steady_clock::now() + 3s).ok());
        }
        if (second_) {
            EXPECT_TRUE(second_->establish_control_fence().ok());
            EXPECT_TRUE(second_->close_admission(8).ok());
            auto drained = second_->drain(soon());
            EXPECT_TRUE(drained.ok());
            if (drained.ok() && drained.value()) {
                commit(std::move(*drained.value()));
            }
            EXPECT_TRUE(second_->cleanup(soon()).ok());
            EXPECT_TRUE(second_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
            second_.reset();
        }
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
        service_.reset();
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

    rodaq::Result<void> artifact_commit(const pipeline::SealedPipelineSegment& sealed) {
        const auto id = sealed.segment->metadata.segment_id();
        auto files = artifact::SealedFiles::open(
            volume_, id, sealed.files, sealed.envelope, sealed.manifest_limits.max_bytes);
        if (!files.ok()) {
            return files.status();
        }
        auto manifest = sealed.segment->metadata.capture_manifest(files.value()->evidence());
        if (!manifest.ok()) {
            return manifest.status();
        }
        auto reservation = sealed.segment->capacity.handoff_sealed(files.value()->storage_bytes());
        if (!reservation.ok()) {
            return reservation.status();
        }
        auto committer = artifact::SegmentCommitter::create(
            std::move(files.value()), std::move(manifest.value()), sealed.manifest_limits,
            std::move(reservation.value()));
        if (!committer.ok()) {
            return committer.status();
        }
        auto actual = committer.value()->commit(*ledger_);
        if (!actual.ok()) {
            commits_.push_back(std::move(committer.value()));
            return actual.status();
        }
        // Component Artifact sink: publish verified Ledger facts into the
        // test's bounded projection, then actually transfer the retained charge.
        // Production ArtifactService/query/lease integration remains separate.
        published_.push_back(committer.value()->ledger_record());
        auto charge = committer.value()->take_committed_charge();
        if (!charge.ok()) {
            commits_.push_back(std::move(committer.value()));
            return charge.status();
        }
        charges_.push_back(std::move(charge.value()));
        commits_.push_back(std::move(committer.value()));
        return {};
    }

    void commit(pipeline::SealedPipelineSegment sealed) {
        ASSERT_TRUE(artifact_commit(sealed).ok());
        const auto id = sealed.segment->metadata.segment_id();
        const auto& owner = second_ && id == 8 ? second_ : controller_;
        ASSERT_TRUE(owner->acknowledge_artifact(sealed.work_id, id).ok());
    }

    void open_service(
        std::function<int(rodaq::task::ServiceFileDomain, rodaq::persistence::FileStep)> faults =
            {},
        std::chrono::nanoseconds request_wait = 30s) {
        fs::create_directory(root_ / "service");
        fs::create_directory(root_ / "segment-ids");
        auto ids = artifact::SegmentIdStore::open(
            root_ / "segment-ids", artifact::SegmentIdOpenMode::NewDeployment);
        ASSERT_TRUE(ids.ok());
        segment_ids_ = std::move(ids.value());
        // Actual scheduling domains, independently drainable from the Writer.
        for (unsigned i = 0; i < 4; ++i) {
            auto lane = rodaq::ExecutionLane::create(1, 8);
            ASSERT_TRUE(lane.ok());
            service_lanes_.push_back(std::move(lane.value()));
        }
        rodaq::task::ServiceOptions options;
        options.root = root_ / "service";
        options.plugin_manager = manager_;
        options.file_faults = std::move(faults);
        options.request_wait = request_wait;
        options.max_tasks = 1;
        options.create_lane = service_lanes_[2];
        options.control_lane = service_lanes_[3];
        options.resource_profiles = {"basic"};
        options.output_roots = {"local"};
        options.allowed_plugins = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        auto execution = std::make_shared<rodaq::task::RunExecutionOptions>();
        execution->preparation_lane = service_lanes_[0];
        execution->cleanup_lane = service_lanes_[1];
        execution->resolve = [this](
                                 const auto& control, const auto& config,
                                 auto& identities) -> rodaq::Result<rodaq::task::RunInputs> {
            auto selected = options_;
            selected.definition = config.pipelines[0];
            selected.identity = {
                control.task_id, selected.definition.pipeline_id,
                pipeline::new_instance_id().value(), control.control_generation};
            selected.streams.clear();
            for (const auto& stream : selected.definition.streams) {
                auto records = identities.reserve(stream.stream_id, 1000);
                if (!records.ok()) {
                    return records.status();
                }
                auto descriptors = identities.reserve_descriptors(stream.stream_id, 1000);
                if (!descriptors.ok()) {
                    return descriptors.status();
                }
                selected.streams.push_back(
                    {stream.stream_id, stream.stream_key, 8, records.value(), descriptors.value()});
            }
            auto segment = segment_ids_->reserve(1);
            if (!segment.ok()) {
                return segment.status();
            }
            selected.manifest.task_id = control.task_id;
            selected.manifest.pipeline_id = selected.identity.pipeline_id;
            selected.manifest.instance_id = pipeline::to_string(selected.identity.instance_id);
            selected.manifest.control_generation = control.control_generation;
            selected.manifest.reset_generation_at_commit = control.reset_generation;
            selected.manifest.segment_id = segment.value().begin;
            selected.manifest.config_version = control.active_config_version;
            // This resolver is a component Deployment seam; production effective
            // configuration resolution remains part of DAQ composition.
            selected.manifest.effective_config_id =
                rodaq::persistence::semantic_digest(
                    {{"config_version", control.active_config_version},
                     {"profile", "behavioral-fake.v1"},
                     {"volume_id", artifact::to_string(volume_->identity())}},
                    "rodaq.effective-config.v1")
                    .value();
            const auto effective = selected.manifest.effective_config_id;
            auto controller = pipeline::PipelineController::create(std::move(selected));
            if (!controller.ok()) {
                return controller.status();
            }
            {
                std::lock_guard<std::mutex> registered(resolved_mutex_);
                controller_ = controller.value();
                resolved_ = true;
                resolved_changed_.notify_all();
            }
            return rodaq::task::RunInputs{
                {{std::move(controller.value()), effective, config.pipelines[0].required}},
                boot(),
                &receive_clock,
                0};
        };
        execution->commit = [this](const auto& sealed) { return artifact_commit(sealed); };
        options.execution = std::move(execution);
        auto service = rodaq::TaskService::open(std::move(options));
        ASSERT_TRUE(service.ok());
        service_ = std::move(service.value());
    }

    bool wait_operation(rodaq::TaskId task, rodaq::OperationId operation) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        do {
            auto actual = service_->get_operation(task, operation);
            if (actual.ok() && rodaq::task::terminal(actual.value().state)) {
                return actual.value().state == rodaq::OperationState::Succeeded;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }

    rodaq::TaskConfig formal_config() {
        rodaq::TaskConfig config;
        config.name = "formal-lifecycle";
        config.segment_policy.max_duration = 60s;
        auto definition = options_.definition;
        definition.pipeline_id = 0;
        definition.resource_profile = "basic";
        definition.output_root = "local";
        for (auto& stream : definition.streams) {
            stream.stream_id = 0;
        }
        config.pipelines.push_back(std::move(definition));
        return config;
    }

    rodaq::task::TaskControlRecord accepted_run() {
        rodaq::task::TaskControlRecord initial;
        initial.task_id = options_.identity.task_id;
        initial.active_config_version = options_.manifest.config_version;
        initial.control_generation = 6;
        initial.record_revision = 6;
        auto owner = rodaq::task::TaskController::create(initial).value();
        auto decision =
            owner
                ->request_lifecycle(
                    rodaq::OperationKind::Start, {"run"}, {}, rodaq::WallTime{1s}, {true, true})
                .value();
        // Controller-generated valid candidate at the Task Run work boundary.
        // Formal TaskService durable acceptance is tested separately.
        return *decision.work->candidate;
    }

    void second(bool fail_prepare) {
        auto lane = rodaq::ExecutionLane::create(1, 8);
        ASSERT_TRUE(lane.ok());
        second_lane_ = std::move(lane.value());
        auto options = options_;
        options.writer_lane = second_lane_;
        options.identity.pipeline_id = 4;
        options.definition.pipeline_id = 4;
        for (std::size_t i = 0; i < options.streams.size(); ++i) {
            options.streams[i].stream_id += 2;
            options.definition.streams[i].stream_id += 2;
        }
        options.identity.instance_id =
            pipeline::parse_instance_id("31234567-89ab-4cde-8fab-0123456789ab").value();
        options.manifest.pipeline_id = 4;
        options.manifest.instance_id = pipeline::to_string(options.identity.instance_id);
        options.manifest.segment_id = 8;
        if (fail_prepare) {
            options.definition.acquisition.json =
                R"({"streams":["first","second"],"mode":"fail_prepare"})";
        }
        auto actual = pipeline::PipelineController::create(std::move(options));
        ASSERT_TRUE(actual.ok());
        second_ = std::move(actual.value());
    }

    auto recovery(rodaq::persistence::FileFaultHook faults = {}) {
        return std::shared_ptr<rodaq::task::TaskRecoveryStore>(
            std::move(rodaq::task::TaskRecoveryStore::open(
                          root_ / "recovery", options_.identity.task_id, {}, std::move(faults))
                          .value()));
    }

    rodaq::time::BootIdentity boot() {
        return rodaq::time::parse_boot_identity("11234567-89ab-4cde-8fab-0123456789ab").value();
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
    std::shared_ptr<pipeline::PipelineController> second_;
    std::shared_ptr<rodaq::ExecutionLane> second_lane_;
    std::vector<artifact::LedgerRecord> published_;
    std::vector<artifact::ArtifactCharge> charges_;
    std::vector<std::unique_ptr<artifact::SegmentCommitter>> commits_;
    std::unique_ptr<artifact::SegmentIdStore> segment_ids_;
    std::vector<std::shared_ptr<rodaq::ExecutionLane>> service_lanes_;
    std::unique_ptr<rodaq::TaskService> service_;
    std::mutex resolved_mutex_;
    std::condition_variable resolved_changed_;
    bool resolved_{false};
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

TEST_F(PipelineControllerTest, TaskAllRequiredBarrierFailureNeverOpensAnyPreparedPipeline) {
    create();
    second(true);
    auto control = accepted_run();
    auto run = rodaq::task::RunAuthorization::create(
                   control, rodaq::StartPolicyMode::AllRequired,
                   {{controller_, options_.manifest.effective_config_id, true},
                    {second_, options_.manifest.effective_config_id, true}},
                   recovery(), boot(), &receive_clock)
                   .value();
    auto result = run->prepare(soon());
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.value().barrier_passed);
    EXPECT_EQ(result.value().pipelines[0], StatusCode::Ok);
    EXPECT_NE(result.value().pipelines[1], StatusCode::Ok);
    EXPECT_FALSE(run->publish_authorization(0).ok());
    EXPECT_FALSE(run->open(control).ok());
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
    EXPECT_EQ(second_->snapshot().value().records.accepted, 0);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
}

TEST_F(PipelineControllerTest, TaskBestEffortPublishesWholeEligibleSetBeforeOpeningReadyPipeline) {
    create();
    second(true);
    auto store = recovery();
    auto control = accepted_run();
    auto run = rodaq::task::RunAuthorization::create(
                   control, rodaq::StartPolicyMode::BestEffort,
                   {{controller_, options_.manifest.effective_config_id, true},
                    {second_, options_.manifest.effective_config_id, true}},
                   store, boot(), &receive_clock)
                   .value();
    ASSERT_TRUE(run->prepare(soon()).value().barrier_passed);
    EXPECT_FALSE(run->open(control).ok());
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
    ASSERT_TRUE(run->publish_authorization(0).ok());
    auto durable = store->load().value();
    ASSERT_TRUE(durable.run_operation);
    EXPECT_EQ(durable.run_operation->phase, rodaq::task::RunPhase::OpenAuthorized);
    ASSERT_EQ(durable.run_operation->authorized_pipelines.size(), 1);
    EXPECT_EQ(durable.run_operation->authorized_pipelines[0].pipeline_id, 3);
    EXPECT_FALSE(durable.run_operation->authorized_pipelines[0].opened_observed);
    ASSERT_TRUE(run->open(control).ok());
    ASSERT_TRUE(wait_written(2));
    EXPECT_FALSE(second_->snapshot().value().gate.open);
    EXPECT_EQ(second_->snapshot().value().records.accepted, 0);
    ASSERT_TRUE(run->fence().ok());
    EXPECT_FALSE(run->open(control).ok());
}

TEST_F(PipelineControllerTest, TaskFullAuthorizationAtomicallyContainsBothReadyInstances) {
    create();
    second(false);
    auto store = recovery();
    auto control = accepted_run();
    auto run = rodaq::task::RunAuthorization::create(
                   control, rodaq::StartPolicyMode::AllRequired,
                   {{controller_, options_.manifest.effective_config_id, true},
                    {second_, options_.manifest.effective_config_id, true}},
                   store, boot(), &receive_clock)
                   .value();
    ASSERT_TRUE(run->prepare(soon()).value().barrier_passed);
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
    EXPECT_EQ(second_->snapshot().value().records.accepted, 0);
    ASSERT_TRUE(run->publish_authorization(0).ok());
    const auto durable = store->load().value();
    ASSERT_EQ(durable.run_operation->authorized_pipelines.size(), 2);
    EXPECT_EQ(durable.run_operation->phase, rodaq::task::RunPhase::OpenAuthorized);
    ASSERT_TRUE(run->open(control).ok());
    EXPECT_TRUE(controller_->snapshot().value().gate.open);
    EXPECT_TRUE(second_->snapshot().value().gate.open);
    ASSERT_TRUE(wait_written(1));
    ASSERT_TRUE(run->fence().ok());
}

TEST_F(PipelineControllerTest, TaskRunRejectsWrongOperationAndLateOpenAfterActualAuthorization) {
    create();
    auto control = accepted_run();
    auto run = rodaq::task::RunAuthorization::create(
                   control, rodaq::StartPolicyMode::AllRequired,
                   {{controller_, options_.manifest.effective_config_id, true}}, recovery(), boot(),
                   &receive_clock)
                   .value();
    ASSERT_TRUE(run->prepare(soon()).value().barrier_passed);
    ASSERT_TRUE(run->publish_authorization(0).ok());
    auto stale = control;
    stale.current_operation_id->value[0] ^= 1;
    EXPECT_FALSE(run->open(stale).ok());
    EXPECT_FALSE(controller_->snapshot().value().gate.open);
    ASSERT_TRUE(run->fence().ok());
    EXPECT_FALSE(run->open(control).ok());
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
}

TEST_F(PipelineControllerTest, UncertainRecoveryAuthorizationCannotGrantGatePermission) {
    create();
    bool armed = false;
    unsigned publications = 0;
    auto store = recovery([&](auto step) {
        return armed && step == rodaq::persistence::FileStep::SyncDirectory && ++publications == 2
                   ? EIO
                   : 0;
    });
    auto control = accepted_run();
    auto run = rodaq::task::RunAuthorization::create(
                   control, rodaq::StartPolicyMode::AllRequired,
                   {{controller_, options_.manifest.effective_config_id, true}}, store, boot(),
                   &receive_clock)
                   .value();
    ASSERT_TRUE(run->prepare(soon()).value().barrier_passed);
    armed = true;
    EXPECT_FALSE(run->publish_authorization(0).ok());
    EXPECT_FALSE(run->open(control).ok());
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
}

TEST_F(PipelineControllerTest, FenceDuringActualAuthorizationPublishDefeatsLateSuccessfulReturn) {
    create();
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false;
    bool entered = false;
    bool released = false;
    unsigned publications = 0;
    auto store = recovery([&](auto step) {
        if (armed && step == rodaq::persistence::FileStep::SyncFile && ++publications == 2) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        return 0;
    });
    auto control = accepted_run();
    auto run = rodaq::task::RunAuthorization::create(
                   control, rodaq::StartPolicyMode::AllRequired,
                   {{controller_, options_.manifest.effective_config_id, true}}, store, boot(),
                   &receive_clock)
                   .value();
    ASSERT_TRUE(run->prepare(soon()).value().barrier_passed);
    armed = true;
    auto actual = std::async(std::launch::async, [&] { return run->publish_authorization(0); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(changed.wait_for(lock, 2s, [&] { return entered; }));
    }
    EXPECT_FALSE(controller_->snapshot().value().gate.open);
    EXPECT_TRUE(run->fence().ok());
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    changed.notify_all();
    EXPECT_FALSE(actual.get().ok());
    EXPECT_EQ(store->load().value().run_operation->phase, rodaq::task::RunPhase::OpenAuthorized);
    EXPECT_FALSE(run->open(control).ok());
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
}

TEST_F(
    PipelineControllerTest, FormalTaskServiceStartPauseResumeStopUsesDurableControlAndRealFiles) {
    open_service();
    ASSERT_TRUE(service_);
    auto config = formal_config();
    auto created = service_->create_task(config, {"create"});
    ASSERT_TRUE(created.ok()) << static_cast<int>(created.status().code());
    auto id = created.value();
    auto started = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(started.ok());
    ASSERT_TRUE(wait_operation(id, started.value()));
    EXPECT_EQ(
        service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Active);
    ASSERT_TRUE(wait_written(4));
    const auto first_instance = controller_->snapshot().value().identity.instance_id;
    auto paused = service_->pause(id, {"pause"}, {true, 2});
    ASSERT_TRUE(paused.ok());
    ASSERT_TRUE(wait_operation(id, paused.value()));
    EXPECT_EQ(service_->get_task(id).value().desired_state, rodaq::DesiredState::Paused);
    EXPECT_EQ(service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    ASSERT_EQ(published_.size(), 1);
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / "1/manifest.json"));
    auto resumed = service_->resume(id, {"resume"}, {true, 3});
    ASSERT_TRUE(resumed.ok());
    ASSERT_TRUE(wait_operation(id, resumed.value()));
    EXPECT_FALSE(controller_->snapshot().value().identity.instance_id == first_instance);
    ASSERT_TRUE(wait_written(3));
    auto stopped = service_->stop(id, {"stop"}, {true, 4});
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(wait_operation(id, stopped.value()));
    EXPECT_EQ(service_->get_task(id).value().desired_state, rodaq::DesiredState::Stopped);
    EXPECT_EQ(service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 5);
    ASSERT_EQ(published_.size(), 2);
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / "2/manifest.json"));
    EXPECT_EQ(service_->start(id, {"start"}, {true, 1}).value(), started.value());
    EXPECT_EQ(service_->stop(id, {"stop"}, {true, 4}).value(), stopped.value());
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 4);
}

TEST_F(PipelineControllerTest, FormalStopDuringPreparationPreventsOldRunFromOpeningAfterReturn) {
    options_.definition.acquisition.json =
        R"({"streams":["first","second"],"mode":"late_start","delay_ms":100})";
    open_service();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(formal_config(), {"create"}).value();
    auto started = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(started.ok());
    {
        std::unique_lock<std::mutex> registered(resolved_mutex_);
        ASSERT_TRUE(resolved_changed_.wait_for(registered, 2s, [&] { return resolved_; }));
    }
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (controller_->snapshot().value().quiescence.control_inflight == 0 &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::yield();
    }
    ASSERT_GT(controller_->snapshot().value().quiescence.control_inflight, 0);
    auto stopped = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(wait_operation(id, stopped.value()));
    EXPECT_EQ(
        service_->get_operation(id, started.value()).value().state,
        rodaq::OperationState::Superseded);
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
    EXPECT_EQ(service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(service_->stop(id, {"stop"}, {true, 2}).value(), stopped.value());
}

TEST_F(PipelineControllerTest, FormalFailedPreparationCleansOwnersAndPreservesRunningIntent) {
    options_.definition.acquisition.json =
        R"({"streams":["first","second"],"mode":"fail_prepare"})";
    open_service();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(formal_config(), {"create"}).value();
    auto started = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(started.ok());
    EXPECT_FALSE(wait_operation(id, started.value()));
    EXPECT_EQ(
        service_->get_operation(id, started.value()).value().state, rodaq::OperationState::Failed);
    EXPECT_EQ(service_->get_task(id).value().desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(
        service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Protected);
    EXPECT_EQ(
        service_->get_operation(id, started.value()).value().data_impact,
        rodaq::DataImpact::PreAdmissionOnly);
    ASSERT_TRUE(controller_);
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
    auto stopped = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(wait_operation(id, stopped.value()));
    EXPECT_EQ(service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
}

TEST_F(
    PipelineControllerTest, FormalRunStatusRetryReusesActualInstanceAfterTerminalPublishFailure) {
    auto armed = std::make_shared<std::atomic<bool>>(false);
    auto writes = std::make_shared<std::atomic<unsigned>>(0);
    auto failed = std::make_shared<std::atomic<bool>>(false);
    open_service([armed, writes, failed](auto domain, auto step) {
        if (*armed && domain == rodaq::task::ServiceFileDomain::Task &&
            step == rodaq::persistence::FileStep::SyncFile && ++*writes == 2) {
            *failed = true;
            return EIO;
        }
        return 0;
    });
    ASSERT_TRUE(service_);
    auto id = service_->create_task(formal_config(), {"create"}).value();
    *armed = true;
    auto started = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(started.ok());
    const auto end = std::chrono::steady_clock::now() + 3s;
    while ((!*failed || service_->get_task(id).value().execution_condition !=
                            rodaq::ExecutionCondition::Protected) &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(*failed);
    ASSERT_EQ(
        service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Protected);
    const auto instance = controller_->snapshot().value().identity.instance_id;
    EXPECT_EQ(service_->start(id, {"start"}, {true, 1}).value(), started.value());
    ASSERT_TRUE(wait_operation(id, started.value()));
    EXPECT_EQ(controller_->snapshot().value().identity.instance_id, instance);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    auto stopped = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(wait_operation(id, stopped.value()));
}

TEST_F(PipelineControllerTest, FormalStopReconcilesUnknownStartAfterObserverTimeout) {
    struct Publication {
        std::atomic<bool> armed{false};
        std::mutex mutex;
        std::condition_variable changed;
        bool entered{false};
        bool released{false};
        std::atomic<bool> reject_directory{false};
        std::atomic<bool> directory_failed{false};
    };
    auto publication = std::make_shared<Publication>();
    open_service(
        [publication](auto domain, auto step) {
            if (domain == rodaq::task::ServiceFileDomain::Task &&
                step == rodaq::persistence::FileStep::SyncFile &&
                publication->armed.exchange(false)) {
                std::unique_lock<std::mutex> lock(publication->mutex);
                publication->entered = true;
                publication->changed.notify_all();
                publication->changed.wait(lock, [&] { return publication->released; });
            }
            if (domain == rodaq::task::ServiceFileDomain::Task &&
                step == rodaq::persistence::FileStep::SyncDirectory &&
                publication->reject_directory) {
                publication->directory_failed = true;
                return EIO;
            }
            return 0;
        },
        20ms);
    ASSERT_TRUE(service_);
    auto created = service_->create_task(formal_config(), {"create"});
    const auto create_deadline = std::chrono::steady_clock::now() + 2s;
    while (!created.ok() && std::chrono::steady_clock::now() < create_deadline &&
           (created.status().code() == StatusCode::DeadlineExceeded ||
            created.status().code() == StatusCode::ResourceExhausted)) {
        created = service_->create_task(formal_config(), {"create"});
    }
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    publication->armed = true;
    publication->reject_directory = true;
    auto starting = std::async(std::launch::async, [&] {
        return service_->start(id, {"start"}, {true, 1});
    });
    {
        std::unique_lock<std::mutex> lock(publication->mutex);
        EXPECT_TRUE(publication->changed.wait_for(lock, 2s, [&] { return publication->entered; }));
    }
    // Returned timeout proves Stop reached its pending-reply path while the
    // old actual publication was still blocked, without relying on a sleep.
    auto observed = service_->stop(id, {"stop"}, {true, 1});
    EXPECT_FALSE(observed.ok());
    EXPECT_EQ(observed.status().code(), StatusCode::DeadlineExceeded);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 1);
    EXPECT_FALSE(controller_);
    {
        std::lock_guard<std::mutex> lock(publication->mutex);
        publication->released = true;
    }
    publication->changed.notify_all();
    EXPECT_EQ(starting.get().status().code(), StatusCode::DeadlineExceeded);
    const auto protected_deadline = std::chrono::steady_clock::now() + 3s;
    while ((!publication->directory_failed || service_->get_task(id).value().execution_condition !=
                                                  rodaq::ExecutionCondition::Protected) &&
           std::chrono::steady_clock::now() < protected_deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(publication->directory_failed);
    ASSERT_EQ(
        service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Protected);
    publication->reject_directory = false;
    auto retried = service_->stop(id, {"stop"}, {true, 1});
    const auto retry_deadline = std::chrono::steady_clock::now() + 3s;
    while (!retried.ok() && retried.status().code() == StatusCode::DeadlineExceeded &&
           std::chrono::steady_clock::now() < retry_deadline) {
        retried = service_->stop(id, {"stop"}, {true, 1});
    }
    ASSERT_TRUE(retried.ok());
    const auto end = std::chrono::steady_clock::now() + 3s;
    auto snapshot = service_->get_task(id).value();
    while ((!snapshot.current_operation ||
            snapshot.current_operation->state != rodaq::OperationState::Succeeded) &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::yield();
        snapshot = service_->get_task(id).value();
    }
    ASSERT_TRUE(snapshot.current_operation);
    EXPECT_EQ(snapshot.current_operation->kind, rodaq::OperationKind::Stop);
    EXPECT_EQ(snapshot.current_operation->state, rodaq::OperationState::Succeeded);
    EXPECT_EQ(snapshot.control_generation, 3);
    EXPECT_EQ(
        service_->stop(id, {"stop"}, {true, 1}).value(), snapshot.current_operation->operation_id);
    auto history = service_->list_operations(id, {}).value();
    ASSERT_EQ(history.items.size(), 2);
    const auto original = std::find_if(
        history.items.begin(), history.items.end(),
        [](const auto& op) { return op.kind == rodaq::OperationKind::Start; });
    ASSERT_NE(original, history.items.end());
    EXPECT_EQ(original->state, rodaq::OperationState::Superseded);
    EXPECT_EQ(service_->start(id, {"start"}, {true, 1}).value(), original->operation_id);
    EXPECT_FALSE(controller_);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
}

}  // namespace
