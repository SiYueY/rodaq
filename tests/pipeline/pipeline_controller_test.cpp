#include "pipeline/pipeline_controller.hpp"
#include "artifact/artifact_catalog.hpp"
#include "task/recovery_store.hpp"
#include "task/run_authorization.hpp"
#include "task/task_controller.hpp"
#include "task/task_service.hpp"
#include "artifact/segment_id_store.hpp"
#include "persistence/json.hpp"
#include "daq/composition.hpp"
#include "daq/run_configuration.hpp"
#include "daq/startup.hpp"
#include "../artifact/manifest_fixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <fstream>
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

struct ClockBlocker {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool released{false};
};

std::atomic<ClockBlocker*> clock_blocker{nullptr};
std::atomic<bool> clock_failed{false};

rodaq::Result<rodaq::time::ClockSample> controlled_clock() {
    if (auto* control = clock_blocker.load()) {
        std::unique_lock<std::mutex> lock(control->mutex);
        control->entered = true;
        control->changed.notify_all();
        control->changed.wait(lock, [&] { return control->released; });
    }
    return clock_failed.load()
               ? rodaq::Result<rodaq::time::ClockSample>(rodaq::Status(StatusCode::Unavailable, {}))
               : receive_clock();
}

plugin::Deadline soon() { return plugin::Deadline::after(2s).value(); }

bool fake_footer_written(const fs::path& directory) {
    auto file = rodaq::persistence::read_file(directory, "records.fake", 100000);
    if (!file.ok() || file.value().size() < 49 || file.value().substr(0, 8) != "RDAQFK1\n") {
        return false;
    }
    const auto& bytes = file.value();
    std::size_t offset = 40;
    while (offset < bytes.size()) {
        const auto tag = static_cast<unsigned char>(bytes[offset]);
        if (tag == 3) {
            return bytes.size() - offset == 9;
        }
        if ((tag != 1 && tag != 2) || bytes.size() - offset < 5) {
            return false;
        }
        std::uint32_t length = 0;
        for (unsigned i = 0; i < 4; ++i) {
            length |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i + 1]))
                      << (8 * i);
        }
        if (length > bytes.size() - offset - 5) {
            return false;
        }
        offset += 5 + length;
    }
    return false;
}

class PipelineControllerTest : public testing::Test {
protected:
    struct ReadControl {
        std::atomic<bool> block{false};
        std::mutex mutex;
        std::condition_variable changed;
        bool entered{false};
        bool released{false};
    };
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
        auto ledger = artifact::LedgerStore::open(
            root_ / "ledger", artifact::LedgerOpenMode::Initialize, 8,
            [fault = ledger_fault_](auto step) {
                return fault->load() && step == rodaq::persistence::FileStep::SyncDirectory ? EIO
                                                                                            : 0;
            });
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
        artifact::CatalogOptions artifacts;
        artifacts.volume = volume_;
        artifacts.ledger = ledger_;
        artifacts.capacity = capacity_;
        artifacts.max_artifacts = 8;
        artifacts.max_read_leases = 4;
        artifacts.max_read_bytes = 64;
        artifacts.manifest = options_.manifest_limits;
        artifacts.read_faults = [control = read_control_](artifact::ReadStep step) {
            if (step == artifact::ReadStep::Reading && control->block.load()) {
                std::unique_lock<std::mutex> lock(control->mutex);
                control->entered = true;
                control->changed.notify_all();
                control->changed.wait(lock, [&] { return control->released; });
            }
            return 0;
        };
        auto catalog = artifact::ArtifactCatalog::create(std::move(artifacts));
        ASSERT_TRUE(catalog.ok());
        catalog_ = std::move(catalog.value());
        auto service = rodaq::ArtifactService::create(catalog_);
        ASSERT_TRUE(service.ok());
        artifacts_ = std::move(service.value());
    }

    void TearDown() override {
        if (composition_) {
            EXPECT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 3s).ok());
            composition_.reset();
        }
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
        artifacts_.reset();
        catalog_.reset();
        if (manager_) {
            EXPECT_TRUE(manager_->shutdown(soon()).ok());
        }
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

    rodaq::Result<std::optional<pipeline::SealedPipelineSegment>> rotate_ready(std::uint64_t next) {
        const auto deadline = soon();
        for (;;) {
            auto rotated = controller_->rotate(next, deadline);
            if (rotated.ok()) {
                return rotated;
            }
            const auto actual = controller_->snapshot().value();
            if (rotated.status().code() != StatusCode::Unavailable ||
                actual.successor_segment_id != next || actual.failure != StatusCode::Ok ||
                deadline.remaining().value() == 0ns) {
                ADD_FAILURE() << "rotation status=" << static_cast<int>(rotated.status().code())
                              << " message=" << rotated.status().message()
                              << " pipeline_failure=" << static_cast<int>(actual.failure)
                              << " successor=" << actual.successor_segment_id
                              << " candidates=" << actual.quiescence.admission_candidates
                              << " pending=" << actual.records.pending_admissions;
                return rotated;
            }
            // Captured pre-cut candidates own their rollback until actual
            // return. Resume the same reservation/ID, never manufacture a cut.
            std::this_thread::sleep_for(1ms);
        }
    }

    rodaq::Result<void> artifact_commit(const pipeline::SealedPipelineSegment& sealed) {
        return catalog_->commit(sealed);
    }

    void commit(pipeline::SealedPipelineSegment sealed) {
        ASSERT_TRUE(artifact_commit(sealed).ok());
        const auto id = sealed.segment->metadata.segment_id();
        const auto& owner = second_ && id == 8 ? second_ : controller_;
        ASSERT_TRUE(owner->acknowledge_artifact(sealed.work_id, id).ok());
    }

    void record_committed() {
        create();
        ASSERT_TRUE(controller_);
        auto prepared = controller_->prepare(soon());
        ASSERT_TRUE(prepared.ok());
        ASSERT_EQ(
            controller_->open_admission(authorize(prepared.value())),
            pipeline::GateOpenResult::Opened);
        ASSERT_TRUE(wait_written(4));
        ASSERT_TRUE(controller_->establish_control_fence().ok());
        ASSERT_TRUE(controller_->close_admission(8).ok());
        auto sealed = controller_->drain(soon());
        ASSERT_TRUE(sealed.ok());
        ASSERT_TRUE(sealed.value());
        commit(std::move(*sealed.value()));
        ASSERT_TRUE(controller_->quiesce(soon()).ok());
    }

    void open_service(
        std::function<int(rodaq::task::ServiceFileDomain, rodaq::persistence::FileStep)> faults =
            {},
        std::chrono::nanoseconds request_wait = 30s, std::size_t max_tasks = 1,
        std::function<void(rodaq::TaskId)> before_resolve = {}) {
        fs::create_directory(root_ / "service");
        fs::create_directory(root_ / "segment-ids");
        auto ids = artifact::SegmentIdStore::open(
            root_ / "segment-ids", artifact::SegmentIdOpenMode::NewDeployment);
        ASSERT_TRUE(ids.ok());
        segment_ids_ = std::move(ids.value());
        // Actual scheduling domains, independently drainable from the Writer.
        for (unsigned i = 0; i < 4; ++i) {
            auto lane = rodaq::ExecutionLane::create(i < 2 ? max_tasks : 1, 8);
            ASSERT_TRUE(lane.ok());
            service_lanes_.push_back(std::move(lane.value()));
        }
        rodaq::task::ServiceOptions options;
        options.root = root_ / "service";
        options.plugin_manager = manager_;
        options.file_faults = std::move(faults);
        options.request_wait = request_wait;
        options.max_tasks = max_tasks;
        options.create_lane = service_lanes_[2];
        options.control_lane = service_lanes_[3];
        options.resource_profiles = {{"basic", options_.envelope.max_segment_physical_bytes}};
        options.output_roots = {"local"};
        options.allowed_plugins = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        auto execution = std::make_shared<rodaq::task::RunExecutionOptions>();
        execution->preparation_lane = service_lanes_[0];
        execution->cleanup_lane = service_lanes_[1];
        auto resources = std::make_shared<rodaq::daq::RunResources>();
        resources->options = composition_options();
        resources->writers = {lane_};
        std::vector<std::shared_ptr<rodaq::daq::RunResources>> task_resources{resources};
        for (std::size_t i = 1; i < max_tasks; ++i) {
            auto writer = rodaq::ExecutionLane::create(1, 8);
            ASSERT_TRUE(writer.ok());
            service_lanes_.push_back(writer.value());
            auto additional = std::make_shared<rodaq::daq::RunResources>();
            additional->options = composition_options();
            additional->writers = {std::move(writer.value())};
            task_resources.push_back(std::move(additional));
        }
        execution->resolve =
            [this, task_resources, before_resolve, tasks = std::vector<rodaq::TaskId>{}](
                const auto& control, const auto& config,
                auto& identities) mutable -> rodaq::Result<rodaq::task::RunInputs> {
            std::shared_ptr<rodaq::daq::RunResources> selected;
            {
                std::lock_guard<std::mutex> registered(resolved_mutex_);
                auto found = std::find(tasks.begin(), tasks.end(), control.task_id);
                if (found == tasks.end()) {
                    if (tasks.size() == task_resources.size()) {
                        return rodaq::Status(StatusCode::ResourceExhausted, {});
                    }
                    tasks.push_back(control.task_id);
                    found = std::prev(tasks.end());
                }
                selected = task_resources[static_cast<std::size_t>(found - tasks.begin())];
            }
            if (before_resolve) {
                before_resolve(control.task_id);
            }
            auto resolved = rodaq::daq::resolve_run(*selected, control, config, identities);
            if (resolved.ok()) {
                std::lock_guard<std::mutex> registered(resolved_mutex_);
                controller_ = resolved.value().pipelines[0].controller;
                resolved_ = true;
                resolved_changed_.notify_all();
            }
            return resolved;
        };
        execution->commit = [this](const auto& sealed) { return artifact_commit(sealed); };
        execution->reserve_segment = [this]() -> rodaq::Result<std::uint64_t> {
            auto ids = segment_ids_->reserve(1);
            return ids.ok() ? rodaq::Result<std::uint64_t>(ids.value().begin)
                            : rodaq::Result<std::uint64_t>(ids.status());
        };
        options.execution = std::move(execution);
        auto service = rodaq::TaskService::open(std::move(options));
        ASSERT_TRUE(service.ok());
        service_ = std::move(service.value());
    }

    rodaq::daq::CompositionOptions composition_options() {
        rodaq::daq::CompositionOptions options;
        options.task_root = root_ / "service";
        options.plugins = manager_;
        options.clock = clock_;
        options.boot = boot();
        options.volume = volume_;
        options.capacity = capacity_;
        options.ledger = ledger_;
        options.segment_ids = segment_ids_;
        options.settings.deployment_generation = 1;
        options.settings.qualified_runtime_profile_id = "behavioral-fake.v1";
        options.settings.output_root = "local";
        options.sample = &receive_clock;
        options.settings.tasks.config.max_pipelines = 1;
        rodaq::daq::ResourceProfile profile;
        profile.name = "basic";
        profile.instance = options_.limits;
        profile.storage = options_.envelope;
        profile.manifest = options_.manifest_limits;
        profile.max_prepared_descriptors = options_.max_prepared_descriptors;
        profile.sequence_reservation = 1000;
        profile.descriptor_reservation = 1000;
        options.settings.profiles.push_back(std::move(profile));
        options.settings.storage = {{"rodaq.fake.storage", "1.0.0", "fake"}};
        options.settings.allowed_plugins = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        return options;
    }

    void open_composition(
        pipeline::ReceiveClock clock = &receive_clock,
        std::chrono::nanoseconds native_timeout = 5s) {
        fs::create_directory(root_ / "service");
        fs::create_directory(root_ / "segment-ids");
        auto ids = artifact::SegmentIdStore::open(
            root_ / "segment-ids", artifact::SegmentIdOpenMode::NewDeployment);
        ASSERT_TRUE(ids.ok());
        segment_ids_ = std::move(ids.value());
        auto options = composition_options();
        options.sample = clock;
        options.settings.native_timeout = native_timeout;
        auto composition = rodaq::daq::Composition::create(std::move(options));
        ASSERT_TRUE(composition.ok());
        composition_ = std::move(composition.value());
    }

    bool wait_composed(rodaq::TaskId task, rodaq::OperationId operation) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        do {
            auto actual = composition_->tasks().get_operation(task, operation);
            if (actual.ok() && rodaq::task::terminal(actual.value().state)) {
                return actual.value().state == rodaq::OperationState::Succeeded;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }

    rodaq::daq::StartupOptions startup_options(rodaq::daq::DeploymentOpenMode mode) {
        rodaq::daq::StartupOptions options;
        options.state_root = root_ / "startup-state";
        options.volume_root = root_ / "startup-volume";
        options.mode = mode;
        options.settings = composition_options().settings;
        options.plugins.search_paths = {root_ / "packages"};
        options.plugins.max_loaded_plugins = 2;
        options.plugins.max_instances = 4;
        options.plugins.max_config_size = 4096;
        options.plugins.shutdown_timeout = 2s;
        options.capacity = {1000000, 1000000, 100, 2000000000, 8, 8, 16};
        options.ledger_records = 8;
        return options;
    }

    void initialize_deployment() {
        ASSERT_TRUE(manager_->shutdown(soon()).ok());
        options_.plugins.reset();
        manager_.reset();
        fs::create_directory(root_ / "startup-state");
        fs::create_directory(root_ / "startup-volume");
        auto composition = rodaq::daq::Composition::open(
            startup_options(rodaq::daq::DeploymentOpenMode::Initialize));
        ASSERT_TRUE(composition.ok()) << static_cast<int>(composition.status().code()) << " "
                                      << composition.status().message();
        composition_ = std::move(composition.value());
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
    std::shared_ptr<std::atomic<bool>> ledger_fault_{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<pipeline::PipelineController> controller_;
    std::shared_ptr<pipeline::PipelineController> second_;
    std::shared_ptr<rodaq::ExecutionLane> second_lane_;
    std::shared_ptr<artifact::ArtifactCatalog> catalog_;
    std::unique_ptr<rodaq::ArtifactService> artifacts_;
    std::shared_ptr<ReadControl> read_control_{std::make_shared<ReadControl>()};
    std::shared_ptr<artifact::SegmentIdStore> segment_ids_;
    std::unique_ptr<rodaq::daq::Composition> composition_;
    std::vector<std::shared_ptr<rodaq::ExecutionLane>> service_lanes_;
    std::unique_ptr<rodaq::TaskService> service_;
    std::mutex resolved_mutex_;
    std::condition_variable resolved_changed_;
    bool resolved_{false};
};

TEST_F(PipelineControllerTest, CompositionRunsFormalLifecycleAndReadsActualCommittedFiles) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto created = service.create_task(formal_config(), {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    std::this_thread::sleep_for(10ms);
    auto pause = service.pause(id, {"pause"}, {true, 2});
    ASSERT_TRUE(pause.ok());
    ASSERT_TRUE(wait_composed(id, pause.value()));
    ASSERT_TRUE(composition_->observe().ok());
    auto page = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().items.size(), 1);
    auto artifact = composition_->artifacts().get_artifact(page.value().items[0].artifact_id);
    ASSERT_TRUE(artifact.ok());
    ASSERT_EQ(artifact.value().files.size(), 1);
    auto read = composition_->artifacts().acquire_read(
        artifact.value().summary.artifact_id, artifact.value().files[0].file_id);
    ASSERT_TRUE(read.ok());
    std::array<std::byte, 8> bytes{};
    EXPECT_EQ(read.value().read_at(0, {bytes.data(), bytes.size()}).value(), bytes.size());
    auto resume = service.resume(id, {"resume"}, {true, 3});
    ASSERT_TRUE(resume.ok());
    ASSERT_TRUE(wait_composed(id, resume.value()));
    std::this_thread::sleep_for(10ms);
    auto stop = service.stop(id, {"stop"}, {true, 4});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 2);
    EXPECT_EQ(service.get_task(id).value().control_generation, 5);
    EXPECT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(service.get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(
        read.value().read_at(0, {bytes.data(), bytes.size()}).status().code(),
        StatusCode::InvalidState);
}

TEST_F(PipelineControllerTest, CompositionShutdownPreservesRunIntentGenerationAndContinuation) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto id = service.create_task(formal_config(), {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    std::this_thread::sleep_for(10ms);
    auto before = service.get_task(id).value();
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    auto after = service.get_task(id).value();
    EXPECT_EQ(after.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(after.control_generation, before.control_generation);
    EXPECT_EQ(after.current_operation->operation_id, start.value());
    EXPECT_EQ(after.current_operation->state, before.current_operation->state);
    EXPECT_EQ(after.execution_condition, rodaq::ExecutionCondition::Protected);
    EXPECT_EQ(service.stop(id, {"stop"}, {}).status().code(), StatusCode::InvalidState);
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 1);
    auto record = rodaq::persistence::read_file(
        root_ / "service/recovery", rodaq::to_string(id) + ".json", 262144);
    ASSERT_TRUE(record.ok());
    auto recovery = rodaq::task::decode_recovery_record(record.value(), {});
    ASSERT_TRUE(recovery.ok());
    ASSERT_TRUE(recovery.value().run_operation);
    EXPECT_EQ(recovery.value().run_operation->phase, rodaq::task::RunPhase::Ended);
    ASSERT_EQ(recovery.value().run_operation->authorized_pipelines.size(), 1);
    EXPECT_TRUE(recovery.value().run_operation->authorized_pipelines[0].continuation_permission);
    EXPECT_TRUE(recovery.value().run_operation->authorized_pipelines[0].ended_boundary);
}

TEST_F(PipelineControllerTest, CompositionEffectiveIdentityBindsActualBinariesAndAllProfileLimits) {
    fs::create_directory(root_ / "segment-ids");
    segment_ids_ = artifact::SegmentIdStore::open(
                       root_ / "segment-ids", artifact::SegmentIdOpenMode::NewDeployment)
                       .value();
    auto deployment = composition_options();
    auto control = accepted_run();
    auto definition = options_.definition;
    definition.resource_profile = "basic";
    definition.output_root = "local";
    auto source = manager_->load("rodaq.fake.acquisition", "1.0.0").value().binary();
    auto storage = manager_->load("rodaq.fake.storage", "1.0.0").value().binary();
    const auto identity = [&](const auto& d, const auto& c, const auto& p, const auto& a,
                              const auto& s) {
        return rodaq::persistence::semantic_digest(
                   rodaq::daq::effective_configuration(d, c, definition, p, a, s).value(),
                   "rodaq.effective-config.v1")
            .value();
    };
    const auto baseline =
        identity(deployment, control, deployment.settings.profiles[0], source, storage);
    auto same = control;
    ++same.control_generation;
    same.current_operation_id.reset();
    EXPECT_EQ(
        identity(deployment, same, deployment.settings.profiles[0], source, storage), baseline);
    auto profile = deployment.settings.profiles[0];
    ++profile.instance.descriptors.max_owned_data_bytes;
    EXPECT_NE(identity(deployment, control, profile, source, storage), baseline);
    profile = deployment.settings.profiles[0];
    ++profile.manifest.max_epochs;
    EXPECT_NE(identity(deployment, control, profile, source, storage), baseline);
    profile = deployment.settings.profiles[0];
    ++profile.sequence_reservation;
    EXPECT_NE(identity(deployment, control, profile, source, storage), baseline);
    ++deployment.settings.deployment_generation;
    EXPECT_NE(
        identity(deployment, control, deployment.settings.profiles[0], source, storage), baseline);
    --deployment.settings.deployment_generation;
    deployment.settings.qualified_runtime_profile_id += ".changed";
    EXPECT_NE(
        identity(deployment, control, deployment.settings.profiles[0], source, storage), baseline);
    storage.sha256 = "sha256:" + std::string(64, '0');
    EXPECT_NE(identity(composition_options(), control, profile, source, storage), baseline);
    auto frozen = rodaq::daq::effective_configuration(
                      composition_options(), control, definition, deployment.settings.profiles[0],
                      source, storage)
                      .value();
    EXPECT_EQ(frozen["acquisition"]["sha256"], source.sha256);
    EXPECT_EQ(frozen["volume_id"], artifact::to_string(volume_->identity()));
}

TEST_F(PipelineControllerTest, CompositionShutdownDeadlineRetainsLatePreparationAndCanBeRetried) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines[0].acquisition.json =
        R"({"streams":["first","second"],"mode":"late_start","delay_ms":300})";
    auto id = composition_->tasks().create_task(config, {"create"}).value();
    auto start = composition_->tasks().start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    std::this_thread::sleep_for(20ms);
    auto observed = composition_->shutdown(std::chrono::steady_clock::now() + 2ms);
    EXPECT_FALSE(observed.ok());
    EXPECT_EQ(observed.status().code(), StatusCode::DeadlineExceeded);
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    const auto actual = composition_->tasks().get_task(id).value();
    EXPECT_EQ(actual.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(actual.control_generation, 2);
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 0);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
}

TEST_F(PipelineControllerTest, CompositionBlockedMaintenanceDoesNotDelayCoreShutdown) {
    open_composition(&controlled_clock);
    ASSERT_TRUE(composition_);
    ClockBlocker control;
    clock_blocker.store(&control);
    auto observer = std::async(std::launch::async, [this] { return composition_->observe(); });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(control.mutex);
        entered = control.changed.wait_for(lock, 2s, [&] { return control.entered; });
    }
    const auto shutdown = composition_->shutdown(std::chrono::steady_clock::now() + 1s);
    {
        std::lock_guard<std::mutex> lock(control.mutex);
        control.released = true;
    }
    control.changed.notify_all();
    const auto observed = observer.get();
    clock_blocker.store(nullptr);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(shutdown.ok());
    EXPECT_EQ(observed.status().code(), StatusCode::InvalidState);
}

TEST_F(PipelineControllerTest, CompositionFailedNativeCreationRequiresActualManagerShutdown) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines[0].acquisition.json = R"({"mode":"invalid-mode"})";
    auto& service = composition_->tasks();
    auto id = service.create_task(config, {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    EXPECT_FALSE(wait_composed(id, start.value()));
    // Required Prepare barrier failure is primary; PendingCleanup's
    // InvalidState must not replace it during subsequent cleanup.
    EXPECT_EQ(
        service.get_operation(id, start.value()).value().status_code, StatusCode::Unavailable);
    EXPECT_NE(service.get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_NE(manager_->phase(), plugin::PluginManagerPhase::Stopped);
    EXPECT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(manager_->phase(), plugin::PluginManagerPhase::Stopped);
    EXPECT_EQ(service.get_task(id).value().desired_state, rodaq::DesiredState::Running);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
}

TEST_F(PipelineControllerTest, CompositionObservationPublishesClockFailureWithoutChangingIntent) {
    open_composition(&controlled_clock);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto id = service.create_task(formal_config(), {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    clock_failed = true;
    auto observed = composition_->observe();
    clock_failed = false;
    EXPECT_EQ(observed.status().code(), StatusCode::Unavailable);
    auto protected_task = service.get_task(id).value();
    EXPECT_EQ(protected_task.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(protected_task.control_generation, 2);
    EXPECT_NE(protected_task.execution_condition, rodaq::ExecutionCondition::Active);
    auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
}

TEST_F(PipelineControllerTest, StartupInitializesActualAuthoritiesAndRecordsWithRealBootClock) {
    initialize_deployment();
    ASSERT_TRUE(composition_);
    EXPECT_TRUE(fs::is_regular_file(root_ / "startup-state/bootstrap.json"));
    EXPECT_TRUE(fs::is_regular_file(root_ / "startup-state/time/playback-anchor.json"));
    auto& service = composition_->tasks();
    auto id = service.create_task(formal_config(), {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    std::this_thread::sleep_for(10ms);
    auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 1);
    composition_.reset();
    // Until S4C supplies verified recovery totals/scanning, retained data is an
    // explicit diagnostic, never a zero-capacity projection or file adoption.
    auto reopened =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    EXPECT_FALSE(reopened.ok());
    EXPECT_EQ(reopened.status().code(), StatusCode::Unavailable);
    EXPECT_TRUE(fs::is_directory(root_ / "startup-volume/.rodaq/artifacts/1"));
}

TEST_F(PipelineControllerTest, MaintenanceSettlesLateCloseWithoutRewritingFailedPauseOrIntent) {
    open_composition(&receive_clock, 30ms);
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines[0].acquisition.json =
        R"({"streams":["first","second"],"mode":"late_shutdown","delay_ms":200})";
    auto& service = composition_->tasks();
    auto id = service.create_task(config, {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    std::this_thread::sleep_for(10ms);
    auto pause = service.pause(id, {"pause"}, {true, 2});
    ASSERT_TRUE(pause.ok());
    EXPECT_FALSE(wait_composed(id, pause.value()));
    auto pending = service.get_task(id).value();
    EXPECT_EQ(pending.execution_condition, rodaq::ExecutionCondition::PendingCleanup);
    EXPECT_EQ(pending.desired_state, rodaq::DesiredState::Paused);
    EXPECT_EQ(pending.control_generation, 3);
    EXPECT_EQ(
        service.get_operation(id, pause.value()).value().state, rodaq::OperationState::Failed);
    const auto end = std::chrono::steady_clock::now() + 2s;
    do {
        ASSERT_TRUE(composition_->observe().ok());
        pending = service.get_task(id).value();
        if (pending.execution_condition == rodaq::ExecutionCondition::Idle) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < end);
    EXPECT_EQ(pending.execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(pending.desired_state, rodaq::DesiredState::Paused);
    EXPECT_EQ(pending.control_generation, 3);
    EXPECT_EQ(
        service.get_operation(id, pause.value()).value().state, rodaq::OperationState::Failed);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 1);
}

TEST_F(PipelineControllerTest, RotationKeepsInstanceAndSequencesWhileOldCommitIsStillRetained) {
    open_service();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(formal_config(), {"create"}).value();
    auto start = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(id, start.value()));
    ASSERT_TRUE(wait_written(4));
    const auto before = controller_->snapshot().value();
    const auto next = segment_ids_->reserve(1).value().begin;
    auto rotated = rotate_ready(next);
    ASSERT_TRUE(rotated.ok());
    ASSERT_TRUE(rotated.value());
    auto old = *rotated.value();
    EXPECT_EQ(old.segment->metadata.segment_id(), before.segment_id);
    const auto old_records = old.segment->metadata.snapshot().accepted;
    ASSERT_TRUE(wait_written(old_records + 4));
    auto active = controller_->snapshot().value();
    EXPECT_TRUE(active.gate.open);
    EXPECT_EQ(active.identity.instance_id, before.identity.instance_id);
    EXPECT_EQ(active.identity.control_generation, before.identity.control_generation);
    EXPECT_EQ(active.segment_id, next);
    auto obsolete = controller_->rotation_due(formal_config().segment_policy, 199);
    ASSERT_TRUE(obsolete.ok());
    EXPECT_FALSE(obsolete.value());
    EXPECT_EQ(active.quiescence.commit_inflight, 1);
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
    EXPECT_EQ(
        controller_->rotate(segment_ids_->reserve(1).value().begin, soon()).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_TRUE(controller_->snapshot().value().gate.open);
    commit(old);
    EXPECT_EQ(controller_->snapshot().value().quiescence.commit_inflight, 0);
    auto stop = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(id, stop.value()));
    EXPECT_EQ(artifacts_->query_artifacts({}).value().items.size(), 2);
    const auto manifest = [&](std::uint64_t segment) {
        auto bytes = rodaq::persistence::read_file(
            volume_->artifacts_directory() / std::to_string(segment), "manifest.json", 65536);
        return artifact::decode_manifest(bytes.value(), options_.manifest_limits).value();
    };
    const auto first = manifest(before.segment_id);
    const auto second = manifest(next);
    EXPECT_EQ(first.identity.segment_sequence, 1);
    EXPECT_EQ(second.identity.segment_sequence, 2);
    EXPECT_EQ(first.identity.instance_id, second.identity.instance_id);
    EXPECT_EQ(first.identity.effective_config_id, second.identity.effective_config_id);
    ASSERT_EQ(first.coverage.size(), second.coverage.size());
    for (std::size_t index = 0; index < first.coverage.size(); ++index) {
        EXPECT_EQ(first.coverage[index].stream_id, second.coverage[index].stream_id);
        EXPECT_LT(first.coverage[index].last_sequence, second.coverage[index].first_sequence);
    }
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 2);
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
}

TEST_F(PipelineControllerTest, CompositionAutomaticallyRotatesAtFrozenRecordEnvelope) {
    options_.envelope.max_segment_records = 6;
    options_.envelope.max_segment_input_bytes = 48;
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto id = service.create_task(formal_config(), {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    const auto end = std::chrono::steady_clock::now() + 2s;
    std::size_t count = 0;
    do {
        ASSERT_TRUE(composition_->observe().ok());
        count = composition_->artifacts().query_artifacts({}).value().items.size();
        if (count >= 3) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    ASSERT_GE(count, 3);
    EXPECT_EQ(service.get_task(id).value().desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(service.get_task(id).value().control_generation, 2);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
    auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(service.get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    auto artifacts = composition_->artifacts().query_artifacts({}).value();
    ASSERT_GE(artifacts.items.size(), 3);
    for (const auto& item : artifacts.items) {
        auto encoded = rodaq::persistence::read_file(
            volume_->artifacts_directory() / std::to_string(item.segment_id), "manifest.json",
            65536);
        auto manifest =
            artifact::decode_manifest(encoded.value(), options_.manifest_limits).value();
        std::uint64_t accepted = 0;
        for (const auto& stream : manifest.coverage) {
            accepted += stream.accepted;
            EXPECT_EQ(stream.accepted, stream.written);
        }
        EXPECT_LE(accepted, 6);
        EXPECT_EQ(item.logical_bytes, accepted * 8);
    }
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
}

TEST_F(PipelineControllerTest, StartupCompositionRotatesByActualMonotonicDuration) {
    initialize_deployment();
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.segment_policy.max_duration = 10ms;
    auto& service = composition_->tasks();
    auto id = service.create_task(config, {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    std::this_thread::sleep_for(15ms);
    const auto end = std::chrono::steady_clock::now() + 2s;
    bool committed = false;
    do {
        ASSERT_TRUE(composition_->observe().ok());
        committed = !composition_->artifacts().query_artifacts({}).value().items.empty();
        if (committed) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    ASSERT_TRUE(committed);
    auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(service.get_task(id).value().control_generation, 3);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
}

TEST_F(
    PipelineControllerTest,
    ExhaustedSuccessorCapacityProtectsGateAndSettlesAcceptedCurrentSegment) {
    options_.envelope.max_segment_records = 6;
    options_.envelope.max_segment_input_bytes = 48;
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto id = service.create_task(formal_config(), {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    const auto end = std::chrono::steady_clock::now() + 3s;
    auto actual = service.get_task(id).value();
    do {
        ASSERT_TRUE(composition_->observe().ok());
        actual = service.get_task(id).value();
        if (actual.execution_condition == rodaq::ExecutionCondition::Protected) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    EXPECT_EQ(actual.execution_condition, rodaq::ExecutionCondition::Protected);
    EXPECT_EQ(actual.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(actual.control_generation, 2);
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 8);
    const auto reserved =
        rodaq::persistence::read_file(root_ / "segment-ids", "segment-identity.json", 1024).value();
    ASSERT_TRUE(composition_->observe().ok());
    ASSERT_TRUE(composition_->observe().ok());
    EXPECT_EQ(
        rodaq::persistence::read_file(root_ / "segment-ids", "segment-identity.json", 1024).value(),
        reserved);
    auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
}

TEST_F(PipelineControllerTest, SilentRotationDoesNotCreateEmptyFilesOrRetainOldReservations) {
    options_.definition.acquisition.json = R"({"streams":["first","second"],"mode":"silent"})";
    open_service();
    auto id = service_->create_task(formal_config(), {"create"}).value();
    auto start = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(id, start.value()));
    for (unsigned index = 0; index < 3; ++index) {
        auto next = segment_ids_->reserve(1).value().begin;
        auto rotated = controller_->rotate(next, soon());
        ASSERT_TRUE(rotated.ok());
        EXPECT_FALSE(rotated.value());
        auto actual = controller_->snapshot().value();
        EXPECT_TRUE(actual.gate.open);
        EXPECT_EQ(actual.records.accepted, 0);
        EXPECT_EQ(actual.segment_id, next);
        EXPECT_EQ(actual.quiescence.commit_inflight, 0);
        EXPECT_EQ(capacity_->snapshot().segment_slots, 1);
        EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
    }
    auto stop = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(id, stop.value()));
    EXPECT_EQ(capacity_->snapshot().segment_slots, 0);
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
}

TEST_F(PipelineControllerTest, PhysicalRolloverThresholdPreservesIndependentHardEnvelope) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.segment_policy.max_physical_bytes = 100001;
    EXPECT_EQ(
        service.create_task(config, {"invalid"}).status().code(), StatusCode::InvalidArgument);
    EXPECT_TRUE(service.list_tasks({}).value().items.empty());
    config.segment_policy.max_physical_bytes = 1000;
    auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    auto start = service.start(created.value(), {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(created.value(), start.value()));
    const auto end = std::chrono::steady_clock::now() + 2s;
    rodaq::ArtifactPage page;
    do {
        ASSERT_TRUE(composition_->observe().ok());
        page = composition_->artifacts().query_artifacts({}).value();
        if (!page.items.empty()) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    ASSERT_FALSE(page.items.empty());
    auto artifact = composition_->artifacts().get_artifact(page.items.front().artifact_id).value();
    ASSERT_EQ(artifact.files.size(), 1);
    EXPECT_GE(artifact.files[0].size, 1000);
    EXPECT_LE(artifact.files[0].size, options_.envelope.max_segment_physical_bytes);
    auto stop = service.stop(created.value(), {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(created.value(), stop.value())) << static_cast<int>(
        service.get_operation(created.value(), stop.value()).value().status_code);
}

TEST_F(PipelineControllerTest, SchemaChangeDuringActualRotationBindsToSuccessor) {
    options_.definition.acquisition.json =
        R"({"streams":["first","second"],"period_us":10000,"schema_change_after":10})";
    options_.definition.storage.json = R"({"mode":"late_seal","delay_ms":300})";
    open_service();
    auto id = service_->create_task(formal_config(), {"create"}).value();
    auto start = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(id, start.value()));
    ASSERT_TRUE(wait_written(2));
    const auto next = segment_ids_->reserve(1).value().begin;
    auto rotation = std::async(std::launch::async, [this, next] { return rotate_ready(next); });
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (!fake_footer_written(volume_->artifacts_directory() / "1.tmp") &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(fake_footer_written(volume_->artifacts_directory() / "1.tmp"));
    auto rotated = rotation.get();
    ASSERT_TRUE(rotated.ok());
    ASSERT_TRUE(rotated.value());
    commit(*rotated.value());
    const auto ready_by = std::chrono::steady_clock::now() + 2s;
    auto facts = controller_->snapshot().value();
    do {
        facts = controller_->snapshot().value();
        if (facts.records.descriptors >= 4 && facts.records.pending_descriptors == 0) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < ready_by);
    EXPECT_EQ(facts.segment_id, next);
    EXPECT_GE(facts.records.descriptors, 4);
    EXPECT_EQ(facts.failure, StatusCode::Ok);
    EXPECT_TRUE(facts.gate.open);
    auto stop = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    EXPECT_TRUE(wait_operation(id, stop.value()));
    EXPECT_EQ(artifacts_->query_artifacts({}).value().items.size(), 2);
}

TEST_F(PipelineControllerTest, StopDuringActualLateSealCannotReopenTheSuccessor) {
    options_.envelope.max_segment_records = 6;
    options_.envelope.max_segment_input_bytes = 48;
    options_.definition.storage.json = R"({"mode":"late_seal","delay_ms":500})";
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto id = service.create_task(formal_config(), {"create"}).value();
    auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    std::this_thread::sleep_for(10ms);
    const auto began = std::chrono::steady_clock::now();
    ASSERT_TRUE(composition_->observe().ok());
    bool entered = false;
    do {
        entered = fake_footer_written(volume_->artifacts_directory() / "1.tmp");
        if (entered) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() - began < 200ms);
    ASSERT_TRUE(entered);
    // Footer creation precedes the injected 500ms actual Native return.
    ASSERT_LT(std::chrono::steady_clock::now() - began, 250ms);
    EXPECT_TRUE(composition_->artifacts().query_artifacts({}).value().items.empty());
    auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    EXPECT_EQ(service.get_task(id).value().desired_state, rodaq::DesiredState::Stopped);
    ASSERT_TRUE(wait_composed(id, stop.value()));
    const auto final = service.get_task(id).value();
    EXPECT_EQ(final.control_generation, 3);
    EXPECT_EQ(final.execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 2);
}

TEST_F(
    PipelineControllerTest,
    StopDrainsBothRotatedHandoffAndFinalSegmentWithoutAnotherUserOperation) {
    open_service();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(formal_config(), {"create"}).value();
    auto start = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(id, start.value()));
    ASSERT_TRUE(wait_written(4));
    auto rotated = rotate_ready(segment_ids_->reserve(1).value().begin);
    ASSERT_TRUE(rotated.ok());
    ASSERT_TRUE(rotated.value());
    const auto old = *rotated.value();
    ASSERT_TRUE(wait_written(old.segment->metadata.snapshot().accepted + 4));
    auto stop = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    const bool completed = wait_operation(id, stop.value());
    if (!completed) {
        const auto actual = controller_->snapshot().value();
        const auto operation = service_->get_operation(id, stop.value()).value();
        const auto catalog = catalog_->snapshot();
        ADD_FAILURE() << "stop status=" << static_cast<int>(operation.status_code)
                      << " impact=" << static_cast<int>(operation.data_impact)
                      << " segment=" << actual.segment_id
                      << " successor=" << actual.successor_segment_id
                      << " artifact_work=" << actual.artifact_work_id
                      << " accepted=" << actual.records.accepted
                      << " written=" << actual.records.written
                      << " pending=" << actual.records.pending_admissions
                      << " unknown=" << actual.records.unknown
                      << " source_closed=" << actual.quiescence.source_quiesced
                      << " storage_closed=" << actual.quiescence.storage_closed
                      << " catalog_visible=" << catalog.visible
                      << " catalog_pending=" << catalog.pending;
    }
    ASSERT_TRUE(completed);
    const auto final = controller_->snapshot().value();
    EXPECT_TRUE(final.quiescence.settled);
    EXPECT_EQ(final.records.accepted, final.records.written);
    EXPECT_EQ(artifacts_->query_artifacts({}).value().items.size(), 2);
    // A late exact completion replays a bounded receipt, never accounts the
    // already retired Segment twice or settles the successor by mistake.
    EXPECT_TRUE(
        controller_->acknowledge_artifact(old.work_id, old.segment->metadata.segment_id()).ok());
    EXPECT_EQ(controller_->snapshot().value().records.accepted, final.records.accepted);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 3);
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 2);
}

TEST_F(PipelineControllerTest, RotationDrainsQueuedAcceptedRecordsAfterTheBackgroundPumpStops) {
    options_.definition.storage.json = R"({"mode":"late_write","delay_ms":25})";
    open_service();
    auto id = service_->create_task(formal_config(), {"create"}).value();
    auto start = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(id, start.value()));
    ASSERT_TRUE(wait_written(4));
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (controller_->snapshot().value().queue.queued_records < 6 &&
           std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_GE(controller_->snapshot().value().queue.queued_records, 6);
    auto rotated = rotate_ready(segment_ids_->reserve(1).value().begin);
    ASSERT_TRUE(rotated.ok());
    ASSERT_TRUE(rotated.value());
    const auto old = rotated.value()->segment->metadata.snapshot();
    EXPECT_EQ(old.pending_admissions, 0);
    EXPECT_EQ(old.accepted, old.written);
    EXPECT_EQ(pipeline::unsettled_record_count(old).value(), 0);
    commit(*rotated.value());
    auto stop = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(id, stop.value()));
    EXPECT_TRUE(controller_->snapshot().value().quiescence.settled);
}

TEST_F(PipelineControllerTest, StartupExclusiveLeasePersistsThroughShutdownUntilOwnerDestruction) {
    initialize_deployment();
    ASSERT_TRUE(composition_);
    auto duplicate =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    EXPECT_FALSE(duplicate.ok());
    EXPECT_EQ(duplicate.status().code(), StatusCode::Conflict);
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    duplicate =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    EXPECT_EQ(duplicate.status().code(), StatusCode::Conflict);
    composition_.reset();
    auto reopened =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    ASSERT_TRUE(reopened.ok());
    composition_ = std::move(reopened.value());
    EXPECT_TRUE(composition_->tasks().list_tasks({}).value().items.empty());
}

TEST_F(PipelineControllerTest, StartupNeverReinitializesMissingClockOrSegmentAuthority) {
    initialize_deployment();
    ASSERT_TRUE(composition_);
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    composition_.reset();
    const auto anchor = root_ / "startup-state/time/playback-anchor.json";
    fs::rename(anchor, root_ / "saved-clock.json");
    auto missing_clock =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    EXPECT_FALSE(missing_clock.ok());
    EXPECT_EQ(missing_clock.status().code(), StatusCode::Unavailable);
    EXPECT_FALSE(fs::exists(anchor));
    fs::rename(root_ / "saved-clock.json", anchor);
    const auto ids = root_ / "startup-state/segment-ids/segment-identity.json";
    fs::rename(ids, root_ / "saved-ids.json");
    auto missing_ids =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    EXPECT_FALSE(missing_ids.ok());
    EXPECT_EQ(missing_ids.status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(ids));
    auto reinitialize =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Initialize));
    EXPECT_FALSE(reinitialize.ok());
    EXPECT_EQ(reinitialize.status().code(), StatusCode::Conflict);
    fs::remove(root_ / "startup-state/.daq.lock");
    auto missing_lease =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    EXPECT_FALSE(missing_lease.ok());
    EXPECT_EQ(missing_lease.status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(root_ / "startup-state/.daq.lock"));
}

TEST_F(PipelineControllerTest, StartupRejectsPriorFactsAndMissingNamespaceWithoutAdoption) {
    ASSERT_TRUE(manager_->shutdown(soon()).ok());
    options_.plugins.reset();
    manager_.reset();
    fs::create_directory(root_ / "startup-state");
    fs::create_directory(root_ / "startup-volume");
    std::ofstream(root_ / "startup-volume/unexplained") << "recording evidence";
    auto refused =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Initialize));
    EXPECT_FALSE(refused.ok());
    EXPECT_EQ(refused.status().code(), StatusCode::Conflict);
    EXPECT_TRUE(fs::exists(root_ / "startup-volume/unexplained"));
    EXPECT_FALSE(fs::exists(root_ / "startup-state/time"));
    fs::remove(root_ / "startup-volume/unexplained");
    auto composition =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Initialize));
    ASSERT_TRUE(composition.ok());
    composition_ = std::move(composition.value());
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    composition_.reset();
    fs::remove_all(root_ / "startup-state/service/recovery");
    auto missing =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(fs::exists(root_ / "startup-state/service/recovery"));
}

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

TEST_F(PipelineControllerTest, TwoTasksOwnIndependentRunControlAndCommittedArtifacts) {
    rodaq::TaskId blocked;
    std::promise<void> entered;
    std::promise<void> release;
    auto allowed = release.get_future().share();
    open_service({}, 30s, 2, [&](rodaq::TaskId id) {
        if (id == blocked) {
            entered.set_value();
            allowed.wait();
        }
    });
    ASSERT_TRUE(service_);
    auto config = formal_config();
    const auto first = service_->create_task(config, {"create-first"}).value();
    const auto second = service_->create_task(config, {"create-second"}).value();
    ASSERT_FALSE(first == second);
    blocked = first;

    auto first_start = std::async(std::launch::async, [&] {
        return service_->start(first, {"start"}, {true, 1});
    });
    const bool preparing = entered.get_future().wait_for(2s) == std::future_status::ready;
    auto second_start = service_->start(second, {"start"}, {true, 1});
    const bool second_ready = second_start.ok() && wait_operation(second, second_start.value());
    const auto first_while_blocked = service_->get_task(first);
    release.set_value();
    auto started = first_start.get();
    EXPECT_TRUE(preparing);
    EXPECT_TRUE(second_ready);
    ASSERT_TRUE(first_while_blocked.ok());
    EXPECT_EQ(
        first_while_blocked.value().execution_condition, rodaq::ExecutionCondition::Preparing);
    ASSERT_TRUE(started.ok());
    ASSERT_TRUE(second_start.ok());
    ASSERT_TRUE(wait_operation(first, started.value()))
        << static_cast<int>(service_->get_operation(first, started.value()).value().status_code);
    ASSERT_TRUE(wait_operation(second, second_start.value())) << static_cast<int>(
        service_->get_operation(second, second_start.value()).value().status_code);
    EXPECT_EQ(
        service_->get_task(first).value().execution_condition, rodaq::ExecutionCondition::Active);
    EXPECT_EQ(
        service_->get_task(second).value().execution_condition, rodaq::ExecutionCondition::Active);

    const auto paused = service_->pause(first, {"pause"}, {true, 2});
    ASSERT_TRUE(paused.ok());
    ASSERT_TRUE(wait_operation(first, paused.value()));
    EXPECT_EQ(service_->get_task(first).value().desired_state, rodaq::DesiredState::Paused);
    EXPECT_EQ(service_->get_task(second).value().desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(service_->get_task(second).value().control_generation, 2);
    EXPECT_EQ(
        service_->get_task(second).value().execution_condition, rodaq::ExecutionCondition::Active);
    EXPECT_EQ(
        service_->get_operation(second, started.value()).status().code(), StatusCode::NotFound);

    const auto second_stop = service_->stop(second, {"stop"}, {true, 2});
    ASSERT_TRUE(second_stop.ok());
    ASSERT_TRUE(wait_operation(second, second_stop.value()));
    const auto first_stop = service_->stop(first, {"stop"}, {true, 3});
    ASSERT_TRUE(first_stop.ok());
    ASSERT_TRUE(wait_operation(first, first_stop.value()));
    EXPECT_EQ(service_->start(first, {"start"}, {true, 1}).value(), started.value());
    EXPECT_EQ(service_->start(second, {"start"}, {true, 1}).value(), second_start.value());
    EXPECT_EQ(service_->list_operations(first, {}).value().items.size(), 3);
    EXPECT_EQ(service_->list_operations(second, {}).value().items.size(), 2);
    const auto artifacts = artifacts_->query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 2);
    EXPECT_TRUE(
        (artifacts.value().items[0].task_id == first &&
         artifacts.value().items[1].task_id == second) ||
        (artifacts.value().items[0].task_id == second &&
         artifacts.value().items[1].task_id == first));
    EXPECT_EQ(
        service_->get_task(first).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(
        service_->get_task(second).value().execution_condition, rodaq::ExecutionCondition::Idle);
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
    ASSERT_EQ(artifacts_->query_artifacts({}).value().items.size(), 1);
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
    ASSERT_EQ(artifacts_->query_artifacts({}).value().items.size(), 2);
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

TEST_F(PipelineControllerTest, ArtifactServiceQueriesAndReadsOnlyRealCommittedFiles) {
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
    record_committed();
    auto page = artifacts_->query_artifacts({});
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().items.size(), 1);
    const auto& summary = page.value().items[0];
    EXPECT_EQ(summary.artifact_id.value, 7);
    EXPECT_EQ(summary.format, "fake");
    EXPECT_EQ(summary.logical_bytes, controller_->snapshot().value().records.written * 8);
    EXPECT_GT(summary.physical_bytes, summary.logical_bytes);
    EXPECT_EQ(capacity_->snapshot().commit_slots, 0);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
    auto info = artifacts_->get_artifact({7}).value();
    ASSERT_FALSE(info.files.empty());
    const auto file = std::find_if(info.files.begin(), info.files.end(), [](const auto& candidate) {
        return candidate.size >= 8;
    });
    ASSERT_NE(file, info.files.end());
    EXPECT_EQ(
        rodaq::parse_artifact_id(rodaq::to_string(summary.artifact_id)).value(),
        summary.artifact_id);
    EXPECT_EQ(
        rodaq::parse_artifact_file_id(rodaq::to_string(file->file_id)).value(), file->file_id);
    EXPECT_FALSE(rodaq::parse_artifact_id("../../7").ok());
    EXPECT_FALSE(rodaq::parse_artifact_file_id("f.01").ok());
    auto reader = artifacts_->acquire_read({7}, file->file_id);
    ASSERT_TRUE(reader.ok());
    EXPECT_EQ(reader.value().size(), file->size);
    std::ifstream input(
        volume_->artifacts_directory() / "7" / file->display_name, std::ios::binary);
    std::array<char, 32> expected{};
    input.seekg(1);
    input.read(expected.data(), static_cast<std::streamsize>(expected.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    std::array<std::byte, 32> actual{};
    auto read = reader.value().read_at(1, {actual.data(), actual.size()});
    ASSERT_TRUE(read.ok());
    ASSERT_EQ(read.value(), count);
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(actual[i], static_cast<std::byte>(static_cast<unsigned char>(expected[i])));
    }
    EXPECT_EQ(reader.value().read_at(file->size, {}).value(), 0);
    reader.value().request_cancel();
    EXPECT_EQ(reader.value().read_at(0, {}).status().code(), StatusCode::InvalidState);
}

TEST_F(PipelineControllerTest, ArtifactReadQuotaAndBlockedLeaseAdmissionRemainBounded) {
    record_committed();
    const auto info = artifacts_->get_artifact({7}).value();
    std::vector<rodaq::ArtifactReadHandle> readers;
    readers.reserve(4);
    for (unsigned i = 0; i < 4; ++i) {
        auto reader = artifacts_->acquire_read({7}, info.files[0].file_id);
        ASSERT_TRUE(reader.ok());
        readers.push_back(std::move(reader.value()));
    }
    EXPECT_EQ(catalog_->snapshot().read_leases, 4);
    EXPECT_EQ(
        artifacts_->acquire_read({7}, info.files[0].file_id).status().code(),
        StatusCode::ResourceExhausted);
    std::array<std::byte, 65> too_large{};
    EXPECT_EQ(
        readers[0].read_at(0, {too_large.data(), too_large.size()}).status().code(),
        StatusCode::InvalidArgument);
    ASSERT_TRUE(catalog_->block_reads({7}).ok());
    EXPECT_FALSE(artifacts_->acquire_read({7}, info.files[0].file_id).ok());
    EXPECT_EQ(
        catalog_->wait_readers({7}, std::chrono::steady_clock::now() + 1ms).status().code(),
        StatusCode::DeadlineExceeded);
    readers.clear();
    EXPECT_TRUE(catalog_->wait_readers({7}, std::chrono::steady_clock::now() + 1s).ok());
    EXPECT_EQ(catalog_->snapshot().read_leases, 0);
}

TEST_F(
    PipelineControllerTest,
    ArtifactCancelledReadRetainsLeaseAfterWrapperDestructionUntilActualReturn) {
    record_committed();
    const auto info = artifacts_->get_artifact({7}).value();
    auto acquired = artifacts_->acquire_read({7}, info.files[0].file_id);
    ASSERT_TRUE(acquired.ok());
    auto reader = std::make_unique<rodaq::ArtifactReadHandle>(std::move(acquired.value()));
    std::array<std::byte, 32> bytes{};
    read_control_->block = true;
    auto actual = std::async(std::launch::async, [raw = reader.get(), &bytes] {
        return raw->read_at(0, {bytes.data(), bytes.size()});
    });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(read_control_->mutex);
        entered = read_control_->changed.wait_for(lock, 2s, [&] { return read_control_->entered; });
    }
    if (entered) {
        EXPECT_EQ(reader->read_at(0, {}).status().code(), StatusCode::Conflict);
        EXPECT_TRUE(catalog_->block_reads({7}).ok());
        catalog_->cancel_readers(rodaq::ArtifactId{7});
        reader.reset();
        EXPECT_EQ(catalog_->snapshot().read_leases, 1);
        EXPECT_EQ(catalog_->snapshot().reads_inflight, 1);
        EXPECT_EQ(
            catalog_->wait_readers({7}, std::chrono::steady_clock::now() + 1ms).status().code(),
            StatusCode::DeadlineExceeded);
    }
    {
        std::lock_guard<std::mutex> lock(read_control_->mutex);
        read_control_->released = true;
    }
    read_control_->changed.notify_all();
    auto returned = actual.get();
    ASSERT_TRUE(entered);
    EXPECT_FALSE(returned.ok());
    EXPECT_EQ(returned.status().code(), StatusCode::InvalidState);
    EXPECT_TRUE(catalog_->wait_readers({7}, std::chrono::steady_clock::now() + 1s).ok());
    EXPECT_EQ(catalog_->snapshot().read_leases, 0);
    EXPECT_EQ(catalog_->snapshot().reads_inflight, 0);
}

TEST_F(
    PipelineControllerTest, ArtifactCatalogRebuildVerifiesLedgerAndIgnoresUnregisteredDirectories) {
    record_committed();
    const auto original = artifacts_->get_artifact({7}).value();
    fs::create_directory(volume_->artifacts_directory() / "999");
    std::ofstream(volume_->artifacts_directory() / "999/manifest.json") << "unregistered";
    artifact::CatalogOptions options;
    options.volume = volume_;
    options.ledger = ledger_;
    options.max_artifacts = 8;
    options.capacity = std::make_shared<artifact::VolumeCapacity>(
        std::move(artifact::VolumeCapacity::create(
                      {1000000, 1000000, 100, 10000, 8, 8, 16}, original.summary.physical_bytes, 1)
                      .value()));
    auto restored = artifact::ArtifactCatalog::create(options).value();
    ASSERT_TRUE(restored->rebuild().ok());
    auto service = rodaq::ArtifactService::create(restored).value();
    ASSERT_EQ(service->query_artifacts({}).value().items.size(), 1);
    auto rebuilt = service->get_artifact({7}).value();
    EXPECT_EQ(rebuilt.summary.logical_bytes, original.summary.logical_bytes);
    EXPECT_EQ(rebuilt.summary.physical_bytes, original.summary.physical_bytes);
    EXPECT_EQ(rebuilt.files[0].file_id, original.files[0].file_id);
    EXPECT_EQ(rebuilt.files[0].etag, original.files[0].etag);
    EXPECT_EQ(service->get_artifact({999}).status().code(), StatusCode::NotFound);
    EXPECT_FALSE(restored->rebuild().ok());
}

TEST_F(PipelineControllerTest, ArtifactChangedCommittedFileIsHiddenAndRebuildReportsDataLoss) {
    record_committed();
    const auto info = artifacts_->get_artifact({7}).value();
    std::ofstream(
        volume_->artifacts_directory() / "7" / info.files[0].display_name,
        std::ios::binary | std::ios::trunc)
        << "corrupt";
    EXPECT_EQ(
        artifacts_->acquire_read({7}, info.files[0].file_id).status().code(), StatusCode::DataLoss);
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
    EXPECT_EQ(artifacts_->get_artifact({7}).status().code(), StatusCode::DataLoss);
    artifact::CatalogOptions options;
    options.volume = volume_;
    options.ledger = ledger_;
    options.max_artifacts = 8;
    options.capacity = std::make_shared<artifact::VolumeCapacity>(
        std::move(artifact::VolumeCapacity::create(
                      {1000000, 1000000, 100, 10000, 8, 8, 16}, info.summary.physical_bytes, 1)
                      .value()));
    auto restored = artifact::ArtifactCatalog::create(options).value();
    EXPECT_EQ(restored->rebuild().status().code(), StatusCode::DataLoss);
    EXPECT_EQ(restored->get({7}).status().code(), StatusCode::DataLoss);
    EXPECT_TRUE(restored->query({}).value().items.empty());
}

TEST_F(PipelineControllerTest, ArtifactKeysetPagesSurviveInsertionAndExcludeDurableDeleteIntent) {
    open_service();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(formal_config(), {"create"}).value();
    const auto episode = [&](unsigned index) {
        auto started =
            service_->start(id, {"start-" + std::to_string(index)}, {true, 1 + index * 2});
        if (!started.ok() || !wait_operation(id, started.value()) || !wait_written(3)) {
            return false;
        }
        auto stopped = service_->stop(id, {"stop-" + std::to_string(index)}, {true, 2 + index * 2});
        return stopped.ok() && wait_operation(id, stopped.value());
    };
    ASSERT_TRUE(episode(0));
    ASSERT_TRUE(episode(1));
    rodaq::ArtifactQuery query;
    query.limit = 1;
    auto first = artifacts_->query_artifacts(query).value();
    ASSERT_EQ(first.items.size(), 1);
    ASSERT_TRUE(first.has_more);
    ASSERT_FALSE(first.next_cursor.empty());
    const auto original = first.items[0];
    ASSERT_TRUE(episode(2));
    query.cursor = first.next_cursor;
    auto next = artifacts_->query_artifacts(query).value();
    ASSERT_EQ(next.items.size(), 1);
    EXPECT_FALSE(next.items[0].artifact_id == original.artifact_id);
    EXPECT_TRUE(
        next.items[0].committed_at < original.committed_at ||
        (next.items[0].committed_at == original.committed_at &&
         next.items[0].artifact_id.value > original.artifact_id.value));
    auto wrong_scope = query;
    wrong_scope.format = "other";
    EXPECT_EQ(
        artifacts_->query_artifacts(wrong_scope).status().code(), StatusCode::InvalidArgument);
    rodaq::ArtifactQuery invalid_range;
    invalid_range.recording_range = rodaq::RecordingRange{0, 100};
    EXPECT_EQ(
        artifacts_->query_artifacts(invalid_range).status().code(), StatusCode::InvalidArgument);
    invalid_range.task_id = id;
    EXPECT_EQ(artifacts_->query_artifacts(invalid_range).value().items.size(), 3);
    const auto target = next.items[0].artifact_id;
    ASSERT_TRUE(catalog_->block_reads(target).ok());
    ASSERT_TRUE(catalog_->wait_readers(target, std::chrono::steady_clock::now() + 1s).ok());
    auto deleting = ledger_->get(target.value).value();
    deleting.state = artifact::LedgerState::Deleting;
    deleting.delete_generation = 7;
    deleting.delete_reason = "retention";
    ASSERT_TRUE(ledger_->advance(deleting, artifact::LedgerState::Committed).ok());
    EXPECT_EQ(artifacts_->get_artifact(target).status().code(), StatusCode::NotFound);
    EXPECT_EQ(artifacts_->query_artifacts({}).value().items.size(), 2);
    EXPECT_EQ(catalog_->snapshot().visible, 2);
    EXPECT_EQ(catalog_->snapshot().pending, 1);
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / std::to_string(target.value)));
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 3);
}

TEST_F(
    PipelineControllerTest, ArtifactInvalidHandoffRetainsRegistrationAndCapacityForActualCleanup) {
    create();
    ASSERT_TRUE(controller_);
    auto prepared = controller_->prepare(soon()).value();
    ASSERT_EQ(controller_->open_admission(authorize(prepared)), pipeline::GateOpenResult::Opened);
    ASSERT_TRUE(wait_written(4));
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    auto drained = controller_->drain(soon());
    ASSERT_TRUE(drained.ok());
    ASSERT_TRUE(drained.value());
    auto malformed = *drained.value();
    ++malformed.files.files[0].physical_size;
    EXPECT_FALSE(catalog_->commit(malformed).ok());
    EXPECT_EQ(catalog_->snapshot().pending, 1);
    EXPECT_EQ(catalog_->snapshot().visible, 0);
    EXPECT_EQ(catalog_->snapshot().commit_inflight, 0);
    EXPECT_EQ(capacity_->snapshot().commit_slots, 1);
    EXPECT_FALSE(controller_->quiesce(soon()).ok());
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / "7.tmp"));
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
}

TEST_F(PipelineControllerTest, ArtifactUncertainLedgerIsUnavailableUntilActualReconciliation) {
    record_committed();
    const auto info = artifacts_->get_artifact({7}).value();
    ASSERT_TRUE(catalog_->block_reads({7}).ok());
    auto deleting = ledger_->get(7).value();
    deleting.state = artifact::LedgerState::Deleting;
    deleting.delete_generation = 8;
    deleting.delete_reason = "retention";
    *ledger_fault_ = true;
    EXPECT_FALSE(ledger_->advance(deleting, artifact::LedgerState::Committed).ok());
    EXPECT_EQ(artifacts_->query_artifacts({}).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(artifacts_->get_artifact({7}).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(
        artifacts_->acquire_read({7}, info.files[0].file_id).status().code(),
        StatusCode::Unavailable);
    EXPECT_EQ(catalog_->snapshot().protection, StatusCode::Unavailable);
    EXPECT_EQ(catalog_->snapshot().read_leases, 0);
    *ledger_fault_ = false;
    ASSERT_TRUE(ledger_->reconcile(7).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
    EXPECT_EQ(catalog_->snapshot().protection, StatusCode::Ok);
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / "7"));
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
}

}  // namespace
