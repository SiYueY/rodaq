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
#include <tuple>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "artifact/recovery_scan.hpp"

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

std::atomic<std::uint64_t> retention_monotonic{200};

rodaq::Result<rodaq::time::ClockSample> retention_clock() {
    const auto monotonic = retention_monotonic.load();
    return rodaq::time::ClockSample{monotonic, static_cast<std::int64_t>(monotonic + 900), 1};
}

struct ClockBlocker {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool released{false};
};

std::atomic<ClockBlocker*> clock_blocker{nullptr};
std::atomic<bool> clock_failed{false};

rodaq::Result<rodaq::time::ClockSample> recoverable_clock() {
    return clock_failed.load()
               ? rodaq::Result<rodaq::time::ClockSample>(rodaq::Status(StatusCode::Unavailable, {}))
               : retention_clock();
}

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
    bool lifecycle_fixture_{false};
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
        if (lifecycle_fixture_) {
            fs::copy(
                RODAQ_PIPELINE_LIFECYCLE_PACKAGE, root_ / "packages/lifecycle",
                fs::copy_options::recursive);
        }
        for (const auto& entry : fs::recursive_directory_iterator(root_ / "packages")) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root_ / "packages", fs::perms::owner_read | fs::perms::owner_exec);
        plugin::PluginManagerOptions plugins;
        plugins.search_paths = {root_ / "packages"};
        plugins.max_loaded_plugins = lifecycle_fixture_ ? 3 : 2;
        plugins.max_instances = 4;
        plugins.max_config_size = 4096;
        plugins.shutdown_timeout = lifecycle_fixture_ ? 10s : 2s;
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
            [fault = ledger_fault_, control = ledger_control_,
             target = ledger_blocked_step_](auto step) {
                if (step == target->load() && control->block.exchange(false)) {
                    std::unique_lock<std::mutex> lock(control->mutex);
                    control->entered = true;
                    control->changed.notify_all();
                    control->changed.wait(lock, [&] { return control->released; });
                }
                return fault->load() && step == rodaq::persistence::FileStep::SyncDirectory ? EIO
                                                                                            : 0;
            });
        ASSERT_TRUE(ledger.ok());
        ledger_ = std::shared_ptr<artifact::LedgerStore>(std::move(ledger.value()));
        capacity_ = std::make_shared<artifact::VolumeCapacity>(std::move(
            artifact::VolumeCapacity::create({1000000, 1000000, 100, 10000, 8, 8, 16, 200, 300, 10})
                .value()));
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
        artifacts.delete_faults = [control = delete_failed_step_, arm = fail_deleted_publication_,
                                   failure = ledger_fault_,
                                   blocking = delete_control_](artifact::DeleteStep step) {
            if (step == artifact::DeleteStep::SyncParent && blocking->block.load()) {
                std::unique_lock<std::mutex> lock(blocking->mutex);
                blocking->entered = true;
                blocking->changed.notify_all();
                blocking->changed.wait(lock, [&] { return blocking->released; });
            }
            if (step == artifact::DeleteStep::SyncParent && arm->exchange(false)) {
                *failure = true;
            }
            return control->load() == static_cast<int>(step) ? EIO : 0;
        };
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

    void record_committed(std::uint64_t recording_base = 0) {
        create();
        ASSERT_TRUE(controller_);
        auto prepared = controller_->prepare(soon());
        ASSERT_TRUE(prepared.ok());
        auto authorization = authorize(prepared.value());
        authorization.recording_base = recording_base;
        ASSERT_EQ(controller_->open_admission(authorization), pipeline::GateOpenResult::Opened);
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
        if (lifecycle_fixture_) {
            options.allowed_plugins.push_back(
                {"com.example.lifecycle", "1.0.0", rodaq::task::AdmissionRole::Acquisition});
        }
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
        execution->sample = &receive_clock;
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
        options.task_file_faults = composed_file_faults_;
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
        pipeline::ReceiveClock clock = &receive_clock, std::chrono::nanoseconds native_timeout = 5s,
        std::size_t max_tasks = 1, std::size_t max_pipelines = 1,
        std::chrono::nanoseconds audit_window = 24h,
        rodaq::task::RuntimeRecoveryPolicy recovery_policy = {}) {
        fs::create_directory(root_ / "service");
        fs::create_directory(root_ / "segment-ids");
        auto ids = artifact::SegmentIdStore::open(
            root_ / "segment-ids", artifact::SegmentIdOpenMode::NewDeployment);
        ASSERT_TRUE(ids.ok());
        segment_ids_ = std::move(ids.value());
        auto options = composition_options();
        options.sample = clock;
        options.settings.native_timeout = native_timeout;
        options.settings.max_tasks = max_tasks;
        options.settings.artifact_audit_window = audit_window;
        options.settings.runtime_recovery = recovery_policy;
        options.settings.tasks.config.max_pipelines = max_pipelines;
        auto composition = rodaq::daq::Composition::create(std::move(options));
        ASSERT_TRUE(composition.ok());
        composition_ = std::move(composition.value());
    }

    rodaq::Result<rodaq::task::TaskRecoveryRecord> composed_recovery(rodaq::TaskId id) {
        auto bytes = rodaq::persistence::read_file(
            root_ / "service/recovery", rodaq::to_string(id) + ".json", 262144);
        return bytes.ok() ? rodaq::task::decode_recovery_record(bytes.value(), {})
                          : rodaq::Result<rodaq::task::TaskRecoveryRecord>(bytes.status());
    }

    bool wait_composed_receive(rodaq::TaskId id, std::uint64_t monotonic) {
        if (!composition_->observe().ok()) {
            return false;
        }
        const auto before = composition_->tasks().get_runtime_snapshot(id).value();
        retention_monotonic = monotonic;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            if (!composition_->observe().ok()) {
                return false;
            }
            const auto current = composition_->tasks().get_runtime_snapshot(id);
            bool ready =
                current.ok() && current.value().pipelines.size() == before.pipelines.size();
            for (std::size_t i = 0; ready && i < before.pipelines.size(); ++i) {
                const auto& row = current.value().pipelines[i];
                ready =
                    row.facts && before.pipelines[i].facts &&
                    row.facts->accepted_records >= before.pipelines[i].facts->accepted_records + 4;
                for (const auto& stream : row.streams) {
                    ready = ready && stream.facts &&
                            stream.facts->last_receive_at ==
                                rodaq::WallTime{std::chrono::nanoseconds{
                                    static_cast<std::int64_t>(monotonic + 900)}};
                }
            }
            if (ready) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    void protect_composed_volume(std::uint64_t monotonic) {
        auto probe = capacity_->begin_space_sample(monotonic);
        ASSERT_TRUE(probe.ok());
        ASSERT_TRUE(capacity_->finish_space_sample(probe.value(), 0, monotonic).ok());
        ASSERT_TRUE(composition_->observe().ok());
    }

    rodaq::Result<rodaq::OperationSnapshot> wait_composed_operation(
        rodaq::TaskId task, rodaq::OperationId operation) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        do {
            auto actual = composition_->tasks().get_operation(task, operation);
            if (actual.ok() && rodaq::task::terminal(actual.value().state)) {
                return actual;
            }
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < end);
        return rodaq::Status(StatusCode::DeadlineExceeded, {});
    }

    bool wait_composed(rodaq::TaskId task, rodaq::OperationId operation) {
        const auto actual = wait_composed_operation(task, operation);
        return actual.ok() && actual.value().state == rodaq::OperationState::Succeeded;
    }

    bool wait_composed_records(rodaq::TaskId task) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        do {
            if (!composition_->observe().ok()) {
                return false;
            }
            const auto runtime = composition_->tasks().get_runtime_snapshot(task);
            if (runtime.ok() && std::all_of(
                                    runtime.value().pipelines.begin(),
                                    runtime.value().pipelines.end(), [](const auto& row) {
                                        return row.facts && row.facts->accepted_records != 0;
                                    })) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < end);
        return false;
    }

    bool wait_composed_condition(rodaq::TaskId task, rodaq::ExecutionCondition expected) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
            if (!composition_->observe().ok()) {
                return false;
            }
            const auto actual = composition_->tasks().get_task(task);
            if (actual.ok() && actual.value().execution_condition == expected) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
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
    std::function<int(rodaq::task::ServiceFileDomain, rodaq::persistence::FileStep)>
        composed_file_faults_;
    std::shared_ptr<artifact::ManagedVolume> volume_;
    std::shared_ptr<artifact::VolumeCapacity> capacity_;
    std::shared_ptr<artifact::LedgerStore> ledger_;
    std::shared_ptr<std::atomic<bool>> ledger_fault_{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<ReadControl> ledger_control_{std::make_shared<ReadControl>()};
    std::shared_ptr<std::atomic<rodaq::persistence::FileStep>> ledger_blocked_step_{
        std::make_shared<std::atomic<rodaq::persistence::FileStep>>(
            rodaq::persistence::FileStep::SyncFile)};
    std::shared_ptr<pipeline::PipelineController> controller_;
    std::shared_ptr<pipeline::PipelineController> second_;
    std::shared_ptr<rodaq::ExecutionLane> second_lane_;
    std::shared_ptr<artifact::ArtifactCatalog> catalog_;
    std::unique_ptr<rodaq::ArtifactService> artifacts_;
    std::shared_ptr<ReadControl> read_control_{std::make_shared<ReadControl>()};
    std::shared_ptr<ReadControl> delete_control_{std::make_shared<ReadControl>()};
    std::shared_ptr<std::atomic<int>> delete_failed_step_{std::make_shared<std::atomic<int>>(-1)};
    std::shared_ptr<std::atomic<bool>> fail_deleted_publication_{
        std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<artifact::SegmentIdStore> segment_ids_;
    std::unique_ptr<rodaq::daq::Composition> composition_;
    std::vector<std::shared_ptr<rodaq::ExecutionLane>> service_lanes_;
    std::unique_ptr<rodaq::TaskService> service_;
    std::mutex resolved_mutex_;
    std::condition_variable resolved_changed_;
    bool resolved_{false};
};

class CleanupSchedulingTest : public PipelineControllerTest {
protected:
    void SetUp() override {
        lifecycle_fixture_ = true;
        PipelineControllerTest::SetUp();
    }
};

TEST_F(CleanupSchedulingTest, SupersedingStopSerializesCleanupWhileSiblingTaskStops) {
    std::future<rodaq::Result<rodaq::OperationId>> first_stop;
    std::future<rodaq::Result<rodaq::OperationId>> sibling_stop;
    struct ShutdownGate {
        fs::path path;
        bool released{false};
        bool release() {
            if (released) {
                return true;
            }
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            do {
                const auto fd = open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) {
                    const char byte = 'x';
                    const auto count = write(fd, &byte, 1);
                    close(fd);
                    released = count == 1;
                    return released;
                }
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }

        ~ShutdownGate() { (void)release(); }
    } gate{root_ / "cleanup.gate"};
    ASSERT_EQ(mkfifo(gate.path.c_str(), 0600), 0);
    open_service({}, 10s, 2);
    ASSERT_TRUE(service_);
    const auto sibling = service_->create_task(formal_config(), {"sibling"}).value();
    const auto running = service_->start(sibling, {"start"}, {true, 1});
    ASSERT_TRUE(running.ok());
    ASSERT_TRUE(wait_operation(sibling, running.value()));
    auto broken = formal_config();
    broken.pipelines[0].acquisition = {
        "com.example.lifecycle", "1.0.0",
        rodaq::persistence::Json{
            {"mode", "shutdown_wait"},
            {"gate", gate.path.string()},
            {"events", (root_ / "cleanup.events").string()}}
            .dump()};
    const auto first = service_->create_task(broken, {"first"}).value();
    const auto starting = service_->start(first, {"start"}, {true, 1});
    ASSERT_TRUE(starting.ok());
    const auto entered_by = std::chrono::steady_clock::now() + 2s;
    while (!fs::exists(gate.path.string() + ".entered") &&
           std::chrono::steady_clock::now() < entered_by) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(fs::exists(gate.path.string() + ".entered"));
    first_stop = std::async(std::launch::async, [&] {
        return service_->stop(first, {"stop"}, {true, 2});
    });
    const auto accepted_by = std::chrono::steady_clock::now() + 2s;
    while (service_->get_task(first).value().control_generation != 3 &&
           std::chrono::steady_clock::now() < accepted_by) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(service_->get_task(first).value().control_generation, 3);
    sibling_stop = std::async(std::launch::async, [&] {
        return service_->stop(sibling, {"stop"}, {true, 2});
    });
    EXPECT_EQ(sibling_stop.wait_for(2s), std::future_status::ready);
    const auto pending = service_->get_task(first).value();
    EXPECT_EQ(pending.execution_condition, rodaq::ExecutionCondition::PendingCleanup);
    EXPECT_TRUE(pending.current_operation);
    if (pending.current_operation) {
        EXPECT_FALSE(rodaq::task::terminal(pending.current_operation->state));
    }
    EXPECT_TRUE(gate.release());
    const auto stopped = first_stop.get();
    const auto sibling_stopped = sibling_stop.get();
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(sibling_stopped.ok());
    EXPECT_TRUE(wait_operation(first, stopped.value()));
    EXPECT_TRUE(wait_operation(sibling, sibling_stopped.value()));
    EXPECT_EQ(
        service_->get_operation(first, starting.value()).value().state,
        rodaq::OperationState::Superseded);
    EXPECT_EQ(
        service_->get_task(first).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(
        service_->get_task(sibling).value().execution_condition, rodaq::ExecutionCondition::Idle);
}

class CompositionPolicyTest : public PipelineControllerTest,
                              public testing::WithParamInterface<std::tuple<bool, bool>> {};

TEST_P(CompositionPolicyTest, FormalPrepareBarrierUsesRequiredAndBestEffortPolicy) {
    const auto [best_effort, failed_required] = GetParam();
    open_composition(&receive_clock, 5s, 1, 2);
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.start_policy =
        best_effort ? rodaq::StartPolicyMode::BestEffort : rodaq::StartPolicyMode::AllRequired;
    auto failed = config.pipelines.front();
    failed.required = failed_required;
    failed.acquisition.json = R"({"streams":["first","second"],"mode":"fail_prepare"})";
    config.pipelines.push_back(std::move(failed));
    auto& service = composition_->tasks();
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    const auto terminal = wait_composed_operation(id, start.value());
    ASSERT_TRUE(terminal.ok());
    const bool allowed = best_effort || !failed_required;
    EXPECT_EQ(
        terminal.value().state,
        allowed ? rodaq::OperationState::PartiallySucceeded : rodaq::OperationState::Failed);
    ASSERT_TRUE(wait_composed_condition(
        id, allowed ? rodaq::ExecutionCondition::Degraded : rodaq::ExecutionCondition::Protected));
    const auto task = service.get_task(id).value();
    EXPECT_EQ(task.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(
        task.execution_condition,
        allowed ? rodaq::ExecutionCondition::Degraded : rodaq::ExecutionCondition::Protected);
    const auto runtime = service.get_runtime_snapshot(id).value();
    ASSERT_EQ(runtime.pipelines.size(), 2);
    ASSERT_TRUE(runtime.pipelines[0].facts);
    ASSERT_TRUE(runtime.pipelines[1].facts);
    EXPECT_EQ(
        runtime.pipelines[0].facts->prepare_status, std::optional<StatusCode>{StatusCode::Ok});
    EXPECT_EQ(
        runtime.pipelines[1].facts->prepare_status,
        std::optional<StatusCode>{StatusCode::InvalidState});
    EXPECT_TRUE(runtime.pipelines[0].facts->effective_config_id);
    EXPECT_FALSE(runtime.pipelines[1].facts->effective_config_id);
    EXPECT_EQ(runtime.pipelines[0].facts->admission_open, allowed);
    EXPECT_FALSE(runtime.pipelines[1].facts->admission_open);
    EXPECT_EQ(runtime.pipelines[1].facts->accepted_records, 0);
    if (!allowed) {
        EXPECT_EQ(runtime.pipelines[0].facts->accepted_records, 0);
        EXPECT_TRUE(composition_->artifacts().query_artifacts({}).value().items.empty());
    }
    EXPECT_EQ(service.start(id, {"start"}, {true, 1}).value(), start.value());
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(service.get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
}

INSTANTIATE_TEST_SUITE_P(
    RequiredAndPolicy, CompositionPolicyTest, testing::Combine(testing::Bool(), testing::Bool()));

TEST_P(CompositionPolicyTest, ConfigurationRejectionFailsOnlyWhenNoLegalRunCanBeRecovered) {
    const auto [best_effort, failed_required] = GetParam();
    open_composition(&receive_clock, 5s, 1, 2);
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.start_policy =
        best_effort ? rodaq::StartPolicyMode::BestEffort : rodaq::StartPolicyMode::AllRequired;
    auto failed = config.pipelines.front();
    failed.required = failed_required;
    failed.acquisition.json = R"({"streams":["first","second"],"unknown_option":true})";
    config.pipelines.push_back(std::move(failed));
    auto& service = composition_->tasks();
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    const auto terminal = wait_composed_operation(id, start.value());
    ASSERT_TRUE(terminal.ok());
    const bool allowed = best_effort || !failed_required;
    EXPECT_EQ(
        terminal.value().state,
        allowed ? rodaq::OperationState::PartiallySucceeded : rodaq::OperationState::Failed);
    ASSERT_TRUE(wait_composed_condition(
        id, allowed ? rodaq::ExecutionCondition::Degraded : rodaq::ExecutionCondition::Failed));
    const auto actual = service.get_task(id).value();
    EXPECT_EQ(actual.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(actual.control_generation, 2);
    EXPECT_EQ(
        actual.execution_condition,
        allowed ? rodaq::ExecutionCondition::Degraded : rodaq::ExecutionCondition::Failed);
    const auto runtime = service.get_runtime_snapshot(id).value();
    ASSERT_EQ(runtime.pipelines.size(), 2);
    ASSERT_TRUE(runtime.pipelines[1].facts);
    EXPECT_EQ(runtime.pipelines[1].facts->prepare_status, StatusCode::InvalidArgument);
    EXPECT_FALSE(runtime.pipelines[1].facts->admission_open);
    EXPECT_EQ(runtime.pipelines[1].facts->accepted_records, 0);
    EXPECT_EQ(service.start(id, {"start"}, {true, 1}).value(), start.value());
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(service.get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
}

TEST_F(PipelineControllerTest, BestEffortWithEveryConfigurationRejectedHasNoRecoverableRun) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.start_policy = rodaq::StartPolicyMode::BestEffort;
    config.pipelines[0].storage.json = R"({"unknown_option":true})";
    auto& service = composition_->tasks();
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto start = service.start(created.value(), {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    const auto terminal = wait_composed_operation(created.value(), start.value());
    ASSERT_TRUE(terminal.ok());
    EXPECT_EQ(terminal.value().state, rodaq::OperationState::Failed);
    ASSERT_TRUE(composition_->observe().ok());
    EXPECT_EQ(
        service.get_task(created.value()).value().execution_condition,
        rodaq::ExecutionCondition::Failed);
    EXPECT_TRUE(composition_->artifacts().query_artifacts({}).value().items.empty());
    const auto stopped = service.stop(created.value(), {"stop-failed"}, {true, 2});
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(wait_composed(created.value(), stopped.value()));
    auto corrected = service.get_active_config(created.value()).value();
    corrected.pipelines[0].storage.json = formal_config().pipelines[0].storage.json;
    const auto configured = service.configure(created.value(), corrected, {"correct"}, {true, 3});
    ASSERT_TRUE(configured.ok());
    ASSERT_TRUE(wait_composed(created.value(), configured.value()));
    const auto restarted = service.start(created.value(), {"start-corrected"}, {true, 4});
    ASSERT_TRUE(restarted.ok());
    ASSERT_TRUE(wait_composed(created.value(), restarted.value()));
    ASSERT_TRUE(wait_composed_records(created.value()));
    EXPECT_EQ(
        service.get_task(created.value()).value().execution_condition,
        rodaq::ExecutionCondition::Active);
    const auto final_stop = service.stop(created.value(), {"stop-corrected"}, {true, 5});
    ASSERT_TRUE(final_stop.ok());
    ASSERT_TRUE(wait_composed(created.value(), final_stop.value()));
}

TEST_F(PipelineControllerTest, PartialRunReleasesFailedPipelineWhileHealthySourceKeepsRecording) {
    open_composition(&receive_clock, 5s, 2, 2);
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    auto failed = config.pipelines.front();
    failed.required = false;
    failed.acquisition.json = R"({"streams":["first","second"],"mode":"fail_prepare"})";
    config.pipelines.push_back(std::move(failed));
    auto& service = composition_->tasks();
    const auto first = service.create_task(config, {"first"}).value();
    const auto start = service.start(first, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    const auto started = wait_composed_operation(first, start.value());
    ASSERT_TRUE(started.ok());
    ASSERT_EQ(started.value().state, rodaq::OperationState::PartiallySucceeded);
    bool settled = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        ASSERT_TRUE(composition_->observe().ok());
        const auto runtime = service.get_runtime_snapshot(first).value();
        settled = runtime.pipelines[1].facts && runtime.pipelines[1].facts->quiescent;
        if (!settled) {
            std::this_thread::sleep_for(1ms);
        }
    } while (!settled && std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(settled);
    EXPECT_EQ(capacity_->snapshot().segment_slots, 1);
    const auto before = service.get_runtime_snapshot(first).value();
    ASSERT_TRUE(before.pipelines[0].facts);
    EXPECT_EQ(before.pipelines[1].facts->accepted_records, 0);
    EXPECT_EQ(service.get_task(first).value().control_generation, 2);
    EXPECT_EQ(
        service.get_task(first).value().execution_condition, rodaq::ExecutionCondition::Degraded);
    EXPECT_EQ(
        service.get_operation(first, start.value()).value().state,
        rodaq::OperationState::PartiallySucceeded);
    const auto sibling = service.create_task(formal_config(), {"sibling"}).value();
    const auto sibling_start = service.start(sibling, {"start"}, {true, 1});
    ASSERT_TRUE(sibling_start.ok());
    ASSERT_TRUE(wait_composed(sibling, sibling_start.value()));
    ASSERT_TRUE(wait_composed_records(sibling));
    ASSERT_TRUE(composition_->observe().ok());
    const auto after = service.get_runtime_snapshot(first).value();
    EXPECT_GT(
        after.pipelines[0].facts->accepted_records, before.pipelines[0].facts->accepted_records);
    const auto first_stop = service.stop(first, {"stop"}, {true, 2});
    const auto sibling_stop = service.stop(sibling, {"stop"}, {true, 2});
    ASSERT_TRUE(first_stop.ok());
    ASSERT_TRUE(sibling_stop.ok());
    ASSERT_TRUE(wait_composed(first, first_stop.value()));
    ASSERT_TRUE(wait_composed(sibling, sibling_stop.value()));
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 2);
}

TEST_F(PipelineControllerTest, PartialRunKeepsPendingCleanupAboveDegradedUntilFailedOwnerSettles) {
    open_composition(&receive_clock, 5s, 1, 2);
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines[0].acquisition.json = R"({"streams":["first","second"],"period_us":1000000})";
    auto failed = config.pipelines.front();
    failed.required = false;
    failed.acquisition.json =
        R"({"streams":["first","first"],"mode":"late_shutdown","delay_ms":1000})";
    config.pipelines.push_back(std::move(failed));
    auto& service = composition_->tasks();
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    const auto terminal = wait_composed_operation(id, start.value());
    ASSERT_TRUE(terminal.ok());
    EXPECT_EQ(terminal.value().state, rodaq::OperationState::PartiallySucceeded);
    ASSERT_FALSE(manager_->initialization_cleanup_settled());
    ASSERT_TRUE(composition_->observe().ok());
    EXPECT_EQ(
        service.get_task(id).value().execution_condition,
        rodaq::ExecutionCondition::PendingCleanup);
    const auto before = service.get_runtime_snapshot(id).value();
    ASSERT_TRUE(before.pipelines[0].facts);
    ASSERT_TRUE(before.pipelines[1].facts);
    EXPECT_TRUE(before.pipelines[0].facts->admission_open);
    EXPECT_FALSE(before.pipelines[1].facts->quiescent);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    bool settled = false;
    do {
        ASSERT_TRUE(composition_->observe().ok());
        const auto current = service.get_runtime_snapshot(id).value();
        settled =
            current.pipelines[0].facts && current.pipelines[1].facts &&
            current.pipelines[0].facts->accepted_records >
                before.pipelines[0].facts->accepted_records &&
            current.pipelines[1].facts->quiescent &&
            service.get_task(id).value().execution_condition == rodaq::ExecutionCondition::Degraded;
        if (!settled) {
            std::this_thread::sleep_for(1ms);
        }
    } while (!settled && std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(settled) << "condition="
                         << static_cast<int>(service.get_task(id).value().execution_condition)
                         << " cleanup_settled=" << manager_->initialization_cleanup_settled()
                         << " segment_slots=" << capacity_->snapshot().segment_slots;
    const auto after = service.get_runtime_snapshot(id).value();
    EXPECT_GT(
        after.pipelines[0].facts->accepted_records, before.pipelines[0].facts->accepted_records);
    EXPECT_EQ(after.pipelines[1].facts->accepted_records, 0);
    EXPECT_EQ(service.get_task(id).value().control_generation, 2);
    EXPECT_EQ(service.get_task(id).value().desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(
        service.get_operation(id, start.value()).value().state,
        rodaq::OperationState::PartiallySucceeded);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
}

TEST_F(PipelineControllerTest, MultiplePipelinesPauseResumeAndCommitTheirOwnAcceptedSets) {
    open_composition(&receive_clock, 5s, 1, 2);
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines.push_back(config.pipelines.front());
    auto& service = composition_->tasks();
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto running = service.get_runtime_snapshot(id).value();
    ASSERT_EQ(running.pipelines.size(), 2);
    EXPECT_NE(running.pipelines[0].pipeline_id, running.pipelines[1].pipeline_id);
    EXPECT_NE(running.pipelines[0].streams[0].stream_id, running.pipelines[1].streams[0].stream_id);
    const auto pause = service.pause(id, {"pause"}, {true, 2});
    ASSERT_TRUE(pause.ok());
    ASSERT_TRUE(wait_composed(id, pause.value()));
    const auto paused = service.get_runtime_snapshot(id).value();
    for (const auto& row : paused.pipelines) {
        ASSERT_TRUE(row.facts);
        EXPECT_TRUE(row.facts->quiescent);
        EXPECT_GT(row.facts->accepted_records, 0);
        EXPECT_EQ(row.facts->accepted_records, row.facts->written_records);
    }
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 2);
    const auto resume = service.resume(id, {"resume"}, {true, 3});
    ASSERT_TRUE(resume.ok());
    ASSERT_TRUE(wait_composed(id, resume.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto resumed = service.get_runtime_snapshot(id).value();
    for (std::size_t i = 0; i < resumed.pipelines.size(); ++i) {
        ASSERT_TRUE(resumed.pipelines[i].facts);
        EXPECT_NE(resumed.pipelines[i].facts->instance_id, paused.pipelines[i].facts->instance_id);
    }
    const auto stop = service.stop(id, {"stop"}, {true, 4});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    const auto ended = service.get_runtime_snapshot(id).value();
    for (const auto& row : ended.pipelines) {
        ASSERT_TRUE(row.facts);
        EXPECT_TRUE(row.facts->quiescent);
        EXPECT_EQ(row.facts->accepted_records, row.facts->written_records);
        EXPECT_EQ(row.facts->data_impact, rodaq::DataImpact::None);
    }
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 4);
}

TEST_F(
    PipelineControllerTest, PluginCapacityRejectionCanStopFailedTaskWhileHealthyTaskKeepsRunning) {
    open_composition(&receive_clock, 5s, 2, 2);
    ASSERT_TRUE(composition_);
    auto busy = formal_config();
    busy.pipelines.push_back(busy.pipelines.front());
    auto& service = composition_->tasks();
    const auto healthy = service.create_task(busy, {"healthy"}).value();
    const auto rejected = service.create_task(formal_config(), {"rejected"}).value();
    const auto start = service.start(healthy, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(healthy, start.value()));
    ASSERT_TRUE(wait_composed_records(healthy));
    const auto exhausted = service.start(rejected, {"start"}, {true, 1});
    ASSERT_TRUE(exhausted.ok());
    const auto failed = wait_composed_operation(rejected, exhausted.value());
    ASSERT_TRUE(failed.ok());
    EXPECT_EQ(failed.value().state, rodaq::OperationState::Failed);
    const auto rejected_runtime = service.get_runtime_snapshot(rejected).value();
    ASSERT_EQ(rejected_runtime.pipelines.size(), 1);
    ASSERT_TRUE(rejected_runtime.pipelines[0].facts);
    EXPECT_EQ(
        rejected_runtime.pipelines[0].facts->prepare_status,
        std::optional<StatusCode>{StatusCode::ResourceExhausted});
    EXPECT_EQ(rejected_runtime.pipelines[0].facts->accepted_records, 0);
    const auto before = service.get_runtime_snapshot(healthy).value();
    const auto stop = service.stop(rejected, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    const auto stopped = wait_composed_operation(rejected, stop.value());
    ASSERT_TRUE(stopped.ok());
    EXPECT_EQ(stopped.value().state, rodaq::OperationState::Succeeded);
    EXPECT_EQ(
        service.get_task(rejected).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(
        service.get_task(healthy).value().execution_condition, rodaq::ExecutionCondition::Active);
    EXPECT_EQ(manager_->phase(), plugin::PluginManagerPhase::Running);
    bool advanced = false;
    const auto limit = std::chrono::steady_clock::now() + 2s;
    do {
        ASSERT_TRUE(composition_->observe().ok());
        const auto after = service.get_runtime_snapshot(healthy).value();
        advanced = after.pipelines[0].facts->accepted_records >
                       before.pipelines[0].facts->accepted_records &&
                   after.pipelines[1].facts->accepted_records >
                       before.pipelines[1].facts->accepted_records;
        if (!advanced) {
            std::this_thread::sleep_for(1ms);
        }
    } while (!advanced && std::chrono::steady_clock::now() < limit);
    EXPECT_TRUE(advanced);
    EXPECT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 3s).ok());
}

TEST_F(PipelineControllerTest, CompositionRejectsUnboundedTaskAndWriterDeploymentCapacity) {
    fs::create_directory(root_ / "service");
    fs::create_directory(root_ / "segment-ids");
    auto ids = artifact::SegmentIdStore::open(
        root_ / "segment-ids", artifact::SegmentIdOpenMode::NewDeployment);
    ASSERT_TRUE(ids.ok());
    segment_ids_ = std::move(ids.value());
    auto options = composition_options();
    options.settings.max_tasks = 0;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.max_tasks = 17;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.max_tasks = 16;
    options.settings.tasks.config.max_pipelines = 17;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.max_tasks = 1;
    options.settings.tasks.config.max_pipelines = 1;
    options.settings.artifact_audit_window = 0ns;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.artifact_audit_window = -1ns;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.artifact_audit_window = 24h;
    const auto recovery_policy = options.settings.runtime_recovery;
    options.settings.runtime_recovery.max_attempts = 0;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.runtime_recovery.max_attempts = 1025;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.runtime_recovery = recovery_policy;
    options.settings.runtime_recovery.initial_backoff = 0ns;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.runtime_recovery = recovery_policy;
    options.settings.runtime_recovery.max_backoff = recovery_policy.initial_backoff - 1ns;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.runtime_recovery = recovery_policy;
    options.settings.runtime_recovery.max_backoff = 25h;
    EXPECT_EQ(
        rodaq::daq::Composition::create(options).status().code(), StatusCode::InvalidArgument);
    options.settings.runtime_recovery = recovery_policy;
    auto valid = rodaq::daq::Composition::create(std::move(options));
    ASSERT_TRUE(valid.ok());
    EXPECT_TRUE(valid.value()->shutdown(std::chrono::steady_clock::now() + 3s).ok());
}

TEST_F(PipelineControllerTest, CompositionAllocatesIndependentWriterSlicesForTwoTasks) {
    open_composition(&receive_clock, 5s, 2);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    const auto first = service.create_task(formal_config(), {"create-first"}).value();
    const auto second = service.create_task(formal_config(), {"create-second"}).value();
    auto one = std::async(std::launch::async, [&] {
        return service.start(first, {"start"}, {true, 1});
    });
    const auto two = service.start(second, {"start"}, {true, 1});
    const auto started = one.get();
    ASSERT_TRUE(started.ok());
    ASSERT_TRUE(two.ok());
    ASSERT_TRUE(wait_composed(first, started.value()));
    ASSERT_TRUE(wait_composed(second, two.value()));
    const auto stopped_first = service.stop(first, {"stop"}, {true, 2});
    ASSERT_TRUE(stopped_first.ok());
    ASSERT_TRUE(wait_composed(first, stopped_first.value()));
    EXPECT_EQ(
        service.get_task(second).value().execution_condition, rodaq::ExecutionCondition::Active);
    const auto stopped_second = service.stop(second, {"stop"}, {true, 2});
    ASSERT_TRUE(stopped_second.ok());
    ASSERT_TRUE(wait_composed(second, stopped_second.value()));
    const auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 2);
    EXPECT_TRUE(
        (artifacts.value().items[0].task_id == first &&
         artifacts.value().items[1].task_id == second) ||
        (artifacts.value().items[0].task_id == second &&
         artifacts.value().items[1].task_id == first));
    EXPECT_EQ(
        service.create_task(formal_config(), {"third"}).status().code(),
        StatusCode::ResourceExhausted);
}

TEST_F(PipelineControllerTest, ConcurrentTaskStopsRetainCommitOwnershipWhileLedgerSyncIsBlocked) {
    open_composition(&receive_clock, 5s, 2);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    const auto first = service.create_task(formal_config(), {"first"}).value();
    const auto second = service.create_task(formal_config(), {"second"}).value();
    const auto first_start = service.start(first, {"start"}, {true, 1});
    const auto second_start = service.start(second, {"start"}, {true, 1});
    ASSERT_TRUE(first_start.ok());
    ASSERT_TRUE(second_start.ok());
    ASSERT_TRUE(wait_composed(first, first_start.value()));
    ASSERT_TRUE(wait_composed(second, second_start.value()));

    ledger_control_->block = true;
    const auto first_stop = service.stop(first, {"stop"}, {true, 2});
    bool inside = false;
    {
        std::unique_lock<std::mutex> lock(ledger_control_->mutex);
        inside =
            ledger_control_->changed.wait_for(lock, 2s, [&] { return ledger_control_->entered; });
    }
    const auto second_stop = service.stop(second, {"stop"}, {true, 2});
    // Give independent Cleanup execution time to reach the occupied Commit
    // domain. Queries and durable control acceptance remain responsive.
    std::this_thread::sleep_for(20ms);
    const auto before_release = second_stop.ok()
                                    ? service.get_operation(second, second_stop.value())
                                    : rodaq::Result<rodaq::OperationSnapshot>(second_stop.status());
    {
        std::lock_guard<std::mutex> lock(ledger_control_->mutex);
        ledger_control_->released = true;
        ledger_control_->changed.notify_all();
    }
    EXPECT_TRUE(inside);
    ASSERT_TRUE(first_stop.ok());
    ASSERT_TRUE(second_stop.ok());
    ASSERT_TRUE(before_release.ok());
    EXPECT_FALSE(rodaq::task::terminal(before_release.value().state));
    ASSERT_TRUE(wait_composed(first, first_stop.value()));
    ASSERT_TRUE(wait_composed(second, second_stop.value()));
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 2);
}

TEST_F(
    PipelineControllerTest, SilentReadySourcePublishesObservedZeroRateWithoutInferringDisconnect) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines[0].acquisition.json = R"({"streams":["first","second"],"mode":"silent"})";
    for (auto& stream : config.pipelines[0].streams) {
        stream.readiness_policy = rodaq::ReadinessPolicy::SourceReady;
    }
    auto& service = composition_->tasks();
    const auto id = service.create_task(config, {"silent"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(composition_->observe().ok());
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(composition_->observe().ok());
    const auto runtime = service.get_runtime_snapshot(id).value();
    ASSERT_EQ(runtime.pipelines.size(), 1);
    ASSERT_TRUE(runtime.pipelines[0].facts);
    EXPECT_TRUE(runtime.pipelines[0].facts->admission_open);
    EXPECT_EQ(runtime.accepted_records_per_second, std::optional<double>{0.0});
    for (const auto& stream : runtime.pipelines[0].streams) {
        ASSERT_TRUE(stream.facts);
        EXPECT_TRUE(stream.facts->entry_ready);
        EXPECT_TRUE(stream.facts->source_ready);
        EXPECT_EQ(stream.facts->receiving, std::optional<bool>{false});
        EXPECT_EQ(stream.facts->received_records_per_second, std::optional<double>{0.0});
        EXPECT_FALSE(stream.facts->last_receive_at);
    }
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
}

TEST_F(PipelineControllerTest, RuntimeQueriesReadPublishedFactsAndRetainTheirObservationIdentity) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    const auto id = service.create_task(formal_config(), {"create"}).value();
    const auto initial = service.get_runtime_snapshot(id);
    ASSERT_TRUE(initial.ok());
    EXPECT_EQ(initial.value().freshness, rodaq::SnapshotFreshness::NotObserved);
    ASSERT_EQ(initial.value().pipelines.size(), 1);
    EXPECT_FALSE(initial.value().pipelines[0].facts);
    ASSERT_EQ(initial.value().pipelines[0].streams.size(), 2);
    EXPECT_FALSE(initial.value().pipelines[0].streams[0].facts);
    EXPECT_EQ(service.get_runtime_snapshot({}).status().code(), StatusCode::NotFound);

    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(composition_->observe().ok());
    const auto captured = service.get_runtime_snapshot(id).value();
    EXPECT_EQ(captured.freshness, rodaq::SnapshotFreshness::Fresh);
    EXPECT_EQ(captured.snapshot_sequence, service.get_task(id).value().snapshot_sequence);
    ASSERT_TRUE(captured.pipelines[0].facts);
    const auto& pipeline = *captured.pipelines[0].facts;
    EXPECT_TRUE(pipeline.admission_open);
    EXPECT_GT(pipeline.accepted_records, 0);
    for (const auto& stream : captured.pipelines[0].streams) {
        ASSERT_TRUE(stream.facts);
        EXPECT_TRUE(stream.facts->entry_ready);
        EXPECT_GT(stream.facts->received_records, 0);
        EXPECT_GE(stream.facts->received_records, stream.facts->accepted_records);
        EXPECT_TRUE(stream.facts->last_receive_at);
    }

    std::this_thread::sleep_for(20ms);
    auto unchanged = service.get_runtime_snapshot(id).value();
    EXPECT_EQ(unchanged.snapshot_sequence, captured.snapshot_sequence);
    EXPECT_EQ(unchanged.captured_at, captured.captured_at);
    EXPECT_EQ(unchanged.pipelines[0].facts->accepted_records, pipeline.accepted_records);
    EXPECT_EQ(service.list_tasks({}).value().items[0].freshness, rodaq::SnapshotFreshness::Fresh);
    ASSERT_TRUE(composition_->observe().ok());
    const auto measured = service.get_runtime_snapshot(id).value();
    EXPECT_TRUE(measured.window);
    EXPECT_TRUE(measured.accepted_records_per_second);
    EXPECT_TRUE(service.list_tasks({}).value().items[0].accepted_records_per_second);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    const auto ended = service.get_runtime_snapshot(id).value();
    ASSERT_TRUE(ended.pipelines[0].facts);
    EXPECT_TRUE(ended.pipelines[0].facts->quiescent);
    EXPECT_EQ(
        ended.pipelines[0].facts->accepted_records, ended.pipelines[0].facts->written_records);
    EXPECT_EQ(ended.data_impact, rodaq::DataImpact::None);
    EXPECT_FALSE(ended.pipelines[0].streams[0].facts->entry_ready);
    std::this_thread::sleep_for(2100ms);
    const auto stale = service.get_runtime_snapshot(id).value();
    EXPECT_EQ(stale.freshness, rodaq::SnapshotFreshness::Stale);
    EXPECT_GE(stale.age, 2s);
    EXPECT_EQ(stale.snapshot_sequence, ended.snapshot_sequence);
    EXPECT_EQ(service.list_tasks({}).value().items[0].freshness, rodaq::SnapshotFreshness::Stale);
    const auto restart = service.start(id, {"restart"}, {true, 3});
    ASSERT_TRUE(restart.ok());
    ASSERT_TRUE(wait_composed(id, restart.value()));
    const auto replacement = service.get_runtime_snapshot(id).value();
    ASSERT_TRUE(replacement.pipelines[0].facts);
    EXPECT_NE(replacement.pipelines[0].facts->instance_id, ended.pipelines[0].facts->instance_id);
    EXPECT_FALSE(replacement.accepted_records_per_second);
    EXPECT_FALSE(replacement.pipelines[0].streams[0].facts->received_records_per_second);
    const auto final_stop = service.stop(id, {"final-stop"}, {true, 4});
    ASSERT_TRUE(final_stop.ok());
    ASSERT_TRUE(wait_composed(id, final_stop.value()));
}

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

TEST_F(PipelineControllerTest, ShutdownPreservesIndependentRunningAndPausedControlAuthorities) {
    open_composition(&receive_clock, 5s, 2);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    std::array<rodaq::TaskId, 2> ids;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto created = service.create_task(formal_config(), {"create-" + std::to_string(i)});
        ASSERT_TRUE(created.ok());
        ids[i] = created.value();
        const auto started = service.start(ids[i], {"start"}, {true, 1});
        ASSERT_TRUE(started.ok());
        ASSERT_TRUE(wait_composed(ids[i], started.value()));
    }
    const auto pause = service.pause(ids[1], {"pause"}, {true, 2});
    ASSERT_TRUE(pause.ok());
    ASSERT_TRUE(wait_composed(ids[1], pause.value()));

    std::array<std::string, 2> controls;
    std::array<rodaq::TaskSnapshot, 2> snapshots;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto bytes = rodaq::persistence::read_file(
            root_ / "service/control", rodaq::to_string(ids[i]) + ".json",
            rodaq::task::control_record_max_bytes({}));
        ASSERT_TRUE(bytes.ok());
        controls[i] = bytes.value();
        snapshots[i] = service.get_task(ids[i]).value();
    }
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 3s).ok());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto bytes = rodaq::persistence::read_file(
            root_ / "service/control", rodaq::to_string(ids[i]) + ".json",
            rodaq::task::control_record_max_bytes({}));
        ASSERT_TRUE(bytes.ok());
        EXPECT_EQ(bytes.value(), controls[i]);
        const auto after = service.get_task(ids[i]);
        ASSERT_TRUE(after.ok());
        EXPECT_EQ(after.value().desired_state, snapshots[i].desired_state);
        EXPECT_EQ(after.value().control_generation, snapshots[i].control_generation);
        ASSERT_TRUE(after.value().current_operation);
        EXPECT_EQ(
            after.value().current_operation->operation_id,
            snapshots[i].current_operation->operation_id);
        const auto history = service.list_operations(ids[i], {});
        ASSERT_TRUE(history.ok());
        EXPECT_EQ(history.value().items.size(), i == 0 ? 1 : 2);
        EXPECT_EQ(
            service.start(ids[i], {"after-shutdown"}, {}).status().code(),
            StatusCode::InvalidState);
    }
    const auto bytes = rodaq::persistence::read_file(
        root_ / "service/recovery", rodaq::to_string(ids[0]) + ".json", 262144);
    ASSERT_TRUE(bytes.ok());
    const auto recovery = rodaq::task::decode_recovery_record(bytes.value(), {});
    ASSERT_TRUE(recovery.ok());
    ASSERT_TRUE(recovery.value().run_operation);
    EXPECT_EQ(recovery.value().run_operation->phase, rodaq::task::RunPhase::Ended);
    ASSERT_EQ(recovery.value().run_operation->authorized_pipelines.size(), 1);
    EXPECT_TRUE(recovery.value().run_operation->authorized_pipelines[0].continuation_permission);
    EXPECT_TRUE(recovery.value().run_operation->authorized_pipelines[0].ended_boundary);
    const auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    EXPECT_EQ(artifacts.value().items.size(), 2);
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
    auto volume_policy = capacity_->policy().value();
    ++volume_policy.low_free_bytes;
    deployment.capacity = std::make_shared<artifact::VolumeCapacity>(
        artifact::VolumeCapacity::create(volume_policy).value());
    EXPECT_NE(
        identity(deployment, control, deployment.settings.profiles[0], source, storage), baseline);
    deployment.capacity = capacity_;
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
    deployment.settings.artifact_audit_window += 1ns;
    EXPECT_NE(
        identity(deployment, control, deployment.settings.profiles[0], source, storage), baseline);
    deployment.settings.artifact_audit_window -= 1ns;
    ++deployment.settings.runtime_recovery.max_attempts;
    EXPECT_NE(
        identity(deployment, control, deployment.settings.profiles[0], source, storage), baseline);
    --deployment.settings.runtime_recovery.max_attempts;
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
    EXPECT_EQ(frozen["volume_admission_limits"]["low_free_bytes"], "200");
    EXPECT_EQ(frozen["volume_admission_limits"]["recovery_free_bytes"], "300");
    EXPECT_EQ(frozen["volume_admission_limits"]["recovery_stable_ns"], "10");
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

TEST_F(PipelineControllerTest, CompositionFailedNativeCreationUsesFrameworkCleanupProof) {
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
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!manager_->initialization_cleanup_settled() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(manager_->initialization_cleanup_settled());
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(service.get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(manager_->phase(), plugin::PluginManagerPhase::Running);
    EXPECT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(manager_->phase(), plugin::PluginManagerPhase::Stopped);
    EXPECT_EQ(service.get_task(id).value().desired_state, rodaq::DesiredState::Stopped);
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
}

TEST_F(
    PipelineControllerTest, LateFailedInitializationMaintenanceReleasesItsCoreSegmentReservation) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines[0].acquisition.json =
        R"({"streams":["first","first"],"mode":"late_shutdown","delay_ms":1000})";
    auto& service = composition_->tasks();
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    const auto failed = wait_composed_operation(id, start.value());
    ASSERT_TRUE(failed.ok());
    EXPECT_EQ(failed.value().state, rodaq::OperationState::Failed);
    EXPECT_FALSE(manager_->initialization_cleanup_settled());
    EXPECT_EQ(capacity_->snapshot().segment_slots, 1);
    EXPECT_EQ(
        service.get_task(id).value().execution_condition,
        rodaq::ExecutionCondition::PendingCleanup);
    const auto limit = std::chrono::steady_clock::now() + 3s;
    while (!manager_->initialization_cleanup_settled() &&
           std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(manager_->initialization_cleanup_settled());
    do {
        ASSERT_TRUE(composition_->observe().ok());
        if (capacity_->snapshot().segment_slots == 0 &&
            service.get_task(id).value().execution_condition == rodaq::ExecutionCondition::Failed) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < limit);
    EXPECT_EQ(capacity_->snapshot().segment_slots, 0);
    EXPECT_EQ(capacity_->snapshot().orphaned_reservations, 0);
    const auto task = service.get_task(id).value();
    EXPECT_EQ(task.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(task.control_generation, 2);
    EXPECT_EQ(task.execution_condition, rodaq::ExecutionCondition::Failed);
    EXPECT_EQ(
        service.get_operation(id, start.value()).value().state, rodaq::OperationState::Failed);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
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
    auto reopened =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());
    const auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 1);
    const auto retained =
        composition_->artifacts().get_artifact(artifacts.value().items[0].artifact_id);
    ASSERT_TRUE(retained.ok());
    ASSERT_FALSE(retained.value().files.empty());
    auto reader = composition_->artifacts().acquire_read(
        retained.value().summary.artifact_id, retained.value().files[0].file_id);
    ASSERT_TRUE(reader.ok());
    std::array<std::byte, 8> header{};
    EXPECT_EQ(reader.value().read_at(0, {header.data(), header.size()}).value(), header.size());
    const auto restarted = composition_->tasks().start(id, {"restart"}, {true, 3});
    ASSERT_TRUE(restarted.ok());
    ASSERT_TRUE(wait_composed(id, restarted.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto stopped = composition_->tasks().stop(id, {"restop"}, {true, 4});
    ASSERT_TRUE(stopped.ok());
    ASSERT_TRUE(wait_composed(id, stopped.value()));
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 2);
    EXPECT_TRUE(fs::is_directory(root_ / "startup-volume/.rodaq/artifacts/1"));
}

TEST_F(PipelineControllerTest, BootCommitRecoveryUsesActualTaskConfigWithoutRewritingFailedStop) {
    open_service();
    ASSERT_TRUE(service_);
    const auto id = service_->create_task(formal_config(), {"create"}).value();
    const auto start = service_->start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(id, start.value()));
    ASSERT_TRUE(wait_written(4));
    *ledger_fault_ = true;
    const auto stop = service_->stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    EXPECT_FALSE(wait_operation(id, stop.value()));
    EXPECT_EQ(
        service_->get_operation(id, stop.value()).value().state, rodaq::OperationState::Failed);
    const auto segment = controller_->snapshot().value().segment_id;
    *ledger_fault_ = false;
    ASSERT_TRUE(ledger_->reconcile(segment).ok());
    ASSERT_EQ(ledger_->get(segment).value().state, artifact::LedgerState::Committing);
    service_.reset();
    for (const auto& lane : service_lanes_) {
        ASSERT_TRUE(lane->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    }
    const auto control_path = root_ / "service/control";
    const auto control_name = rodaq::to_string(id) + ".json";
    const auto before = rodaq::persistence::read_file(control_path, control_name, 1024 * 1024);
    ASSERT_TRUE(before.ok());
    ASSERT_TRUE(rodaq::daq::recover_pending_artifacts(composition_options()).ok());
    EXPECT_EQ(ledger_->get(segment).value().state, artifact::LedgerState::Committed);
    const auto pending = controller_->snapshot().value();
    ASSERT_TRUE(controller_->acknowledge_artifact(pending.artifact_work_id, segment).ok());
    EXPECT_TRUE(controller_->quiesce(soon()).ok());
    EXPECT_TRUE(fake_footer_written(volume_->artifacts_directory() / std::to_string(segment)));
    EXPECT_TRUE(
        artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits).ok());
    const auto after = rodaq::persistence::read_file(control_path, control_name, 1024 * 1024);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value(), before.value());
}

class FormalTombstoneStartupTest : public PipelineControllerTest,
                                   public testing::WithParamInterface<bool> {};

TEST_P(
    FormalTombstoneStartupTest,
    FormalStartupRebuildsDeletedHistoryAndKeepsRecordingTimeAfterAuditGc) {
    initialize_deployment();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    const auto id = service.create_task(formal_config(), {"create"}).value();
    for (unsigned run = 0; run < 2; ++run) {
        const auto generation = service.get_task(id).value().control_generation;
        const auto start = service.start(id, {"start-" + std::to_string(run)}, {true, generation});
        ASSERT_TRUE(start.ok());
        ASSERT_TRUE(wait_composed(id, start.value()));
        ASSERT_TRUE(wait_composed_records(id));
        const auto stop = service.stop(id, {"stop-" + std::to_string(run)}, {true, generation + 1});
        ASSERT_TRUE(stop.ok());
        ASSERT_TRUE(wait_composed(id, stop.value()));
    }
    const auto original = composition_->artifacts().query_artifacts({}).value();
    ASSERT_EQ(original.items.size(), 2);
    const auto latest = std::max(original.items[0].segment_id, original.items[1].segment_id);
    auto config = service.get_active_config(id).value();
    config.recording_mode = rodaq::RecordingMode::Rolling;
    config.retention_policy = {rodaq::RetentionMode::Rolling, {}, 1, true};
    if (GetParam()) {
        config.retention_policy.max_duration = 1ns;
        config.retention_policy.max_physical_bytes.reset();
    }
    const auto configured = service.configure(id, config, {"rolling"}, {true, 5});
    ASSERT_TRUE(configured.ok());
    ASSERT_TRUE(wait_composed(id, configured.value()));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    const std::size_t retained = GetParam() ? 1 : 0;
    while (composition_->artifacts().query_artifacts({}).value().items.size() != retained &&
           std::chrono::steady_clock::now() < deadline) {
        ASSERT_TRUE(composition_->observe().ok());
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), retained);
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    composition_.reset();
    {
        auto ledger = artifact::LedgerStore::open(
            root_ / "startup-state/ledger", artifact::LedgerOpenMode::Existing, 8);
        ASSERT_TRUE(ledger.ok());
        const auto rows = ledger.value()->recovery_snapshot().value();
        ASSERT_EQ(rows.size(), 2);
        EXPECT_EQ(
            std::count_if(
                rows.begin(), rows.end(),
                [](const auto& row) { return row.state == artifact::LedgerState::Committed; }),
            retained);
    }
    const auto checkpoint = [&] {
        auto bytes = rodaq::persistence::read_file(
            root_ / "startup-state/service/recovery", rodaq::to_string(id) + ".json", 1024 * 1024);
        return bytes.ok() ? rodaq::task::decode_recovery_record(bytes.value(), {})
                          : rodaq::Result<rodaq::task::TaskRecoveryRecord>(bytes.status());
    };
    const auto previous = checkpoint();
    ASSERT_TRUE(previous.ok());
    ASSERT_TRUE(previous.value().committed_recording_end_ns);
    auto options = startup_options(rodaq::daq::DeploymentOpenMode::Existing);
    options.settings.artifact_audit_window = 1ns;
    auto reopened = rodaq::daq::Composition::open(std::move(options));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());
    ASSERT_TRUE(composition_->observe().ok());
    EXPECT_EQ(
        checkpoint().value().committed_recording_end_ns,
        previous.value().committed_recording_end_ns);
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), retained);
    auto keep = composition_->tasks().get_active_config(id).value();
    keep.recording_mode = rodaq::RecordingMode::Continuous;
    keep.retention_policy = {};
    const auto keep_all = composition_->tasks().configure(id, keep, {"keep"}, {true, 6});
    ASSERT_TRUE(keep_all.ok());
    ASSERT_TRUE(wait_composed(id, keep_all.value()));
    const auto start = composition_->tasks().start(id, {"restart"}, {true, 7});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto stop = composition_->tasks().stop(id, {"restop"}, {true, 8});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    const auto artifacts = composition_->artifacts().query_artifacts({}).value();
    ASSERT_EQ(artifacts.items.size(), retained + 1);
    EXPECT_GT(artifacts.items[0].segment_id, latest);
    EXPECT_GE(
        artifacts.items[0].recording_range.begin_ns, *previous.value().committed_recording_end_ns);
}

INSTANTIATE_TEST_SUITE_P(DeletedOnlyOrMixedCommitted, FormalTombstoneStartupTest, testing::Bool());

TEST_F(PipelineControllerTest, SuccessfulStopHistoryCannotReplaceMissingEndedRecoveryOnRestart) {
    initialize_deployment();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    const auto id = service.create_task(formal_config(), {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    composition_.reset();
    const auto recovery =
        root_ / "startup-state/service/recovery" / (rodaq::to_string(id) + ".json");
    const auto held = root_ / "held-recovery.json";
    fs::rename(recovery, held);
    for (unsigned i = 0; i < 2; ++i) {
        const auto reopened = rodaq::daq::Composition::open(
            startup_options(rodaq::daq::DeploymentOpenMode::Existing));
        EXPECT_FALSE(reopened.ok());
        EXPECT_EQ(reopened.status().code(), StatusCode::Unavailable);
        EXPECT_FALSE(fs::exists(recovery));
    }
    fs::rename(held, recovery);
    auto restored =
        rodaq::daq::Composition::open(startup_options(rodaq::daq::DeploymentOpenMode::Existing));
    ASSERT_TRUE(restored.ok());
    composition_ = std::move(restored.value());
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 1);
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
        ASSERT_TRUE(manifest.identity.effective_configuration);
        const auto& effective = *manifest.identity.effective_configuration;
        EXPECT_EQ(effective["config_version"], manifest.identity.config_version);
        EXPECT_EQ(
            effective["qualified_runtime_profile_id"],
            manifest.identity.qualified_runtime_profile_id);
        EXPECT_EQ(effective["volume_id"], artifact::to_string(manifest.identity.volume_id));
        EXPECT_EQ(
            effective["effective_pipeline_limits"]["instance"]["queue_capacity"],
            std::to_string(options_.limits.queue_capacity));
        EXPECT_EQ(
            rodaq::persistence::semantic_digest(effective, "rodaq.effective-config.v1").value(),
            manifest.identity.effective_config_id);
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
    EXPECT_FALSE(run->open(control, 200).ok());
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
    EXPECT_FALSE(run->open(control, 200).ok());
    EXPECT_EQ(controller_->snapshot().value().records.accepted, 0);
    ASSERT_TRUE(run->publish_authorization(0).ok());
    auto durable = store->load().value();
    ASSERT_TRUE(durable.run_operation);
    EXPECT_EQ(durable.run_operation->phase, rodaq::task::RunPhase::OpenAuthorized);
    ASSERT_EQ(durable.run_operation->authorized_pipelines.size(), 1);
    EXPECT_EQ(durable.run_operation->authorized_pipelines[0].pipeline_id, 3);
    EXPECT_FALSE(durable.run_operation->authorized_pipelines[0].opened_observed);
    ASSERT_TRUE(run->open(control, 200).ok());
    ASSERT_TRUE(wait_written(2));
    EXPECT_FALSE(second_->snapshot().value().gate.open);
    EXPECT_EQ(second_->snapshot().value().records.accepted, 0);
    ASSERT_TRUE(run->fence().ok());
    EXPECT_FALSE(run->open(control, 200).ok());
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
    ASSERT_TRUE(run->open(control, 200).ok());
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
    EXPECT_FALSE(run->open(stale, 200).ok());
    EXPECT_FALSE(controller_->snapshot().value().gate.open);
    ASSERT_TRUE(run->fence().ok());
    EXPECT_FALSE(run->open(control, 200).ok());
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
    EXPECT_FALSE(run->open(control, 200).ok());
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
    EXPECT_FALSE(run->open(control, 200).ok());
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
        EXPECT_EQ(
            catalog_
                ->delete_artifact(
                    {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                    std::chrono::steady_clock::now() + 1ms)
                .status()
                .code(),
            StatusCode::DeadlineExceeded);
        EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
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
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
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

TEST_F(PipelineControllerTest, StartupVolumeInspectionCountsOnlyFullyVerifiedCommittedBytes) {
    record_committed();
    const auto expected = artifacts_->get_artifact({7}).value().summary.physical_bytes;
    auto usage = artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits);
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage.value().physical_bytes, expected);
    EXPECT_EQ(usage.value().artifact_slots, 1);
    const auto path = volume_->artifacts_directory() / "7";
    fs::rename(path, volume_->artifacts_directory() / "missing");
    EXPECT_FALSE(
        artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits).ok());
    fs::rename(volume_->artifacts_directory() / "missing", path);
    std::ofstream(path / "records.fake", std::ios::binary | std::ios::app) << "corrupt";
    EXPECT_EQ(
        artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits)
            .status()
            .code(),
        StatusCode::DataLoss);
}

TEST_F(PipelineControllerTest, StartupVolumeInspectionNeverAdoptsOrErasesUnregisteredFacts) {
    for (const auto& path :
         {volume_->artifacts_directory() / "999", volume_->artifacts_directory() / "999.tmp",
          volume_->root() / ".rodaq/quarantine/unknown"}) {
        ASSERT_TRUE(fs::create_directory(path));
        EXPECT_EQ(
            artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits)
                .status()
                .code(),
            StatusCode::Unavailable);
        EXPECT_TRUE(fs::is_directory(path));
        EXPECT_TRUE(ledger_->recovery_snapshot().value().empty());
        ASSERT_TRUE(fs::remove(path));
    }
    ASSERT_TRUE(
        artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits).ok());
}

TEST_F(PipelineControllerTest, StartupVolumeInspectionCountsAbsentDeletedMetadataWithoutDataBytes) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    const auto usage =
        artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits);
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage.value().physical_bytes, 0);
    EXPECT_EQ(usage.value().artifact_slots, 1);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
}

class DeleteRecoveryWindowTest : public PipelineControllerTest,
                                 public testing::WithParamInterface<artifact::DeleteStep> {
protected:
    void reopen_ledger_without_runtime_caches() {
        artifacts_.reset();
        catalog_.reset();
        controller_.reset();
        options_.ledger.reset();
        ledger_.reset();
        auto reopened =
            artifact::LedgerStore::open(root_ / "ledger", artifact::LedgerOpenMode::Existing, 8);
        ASSERT_TRUE(reopened.ok());
        ledger_ = std::move(reopened.value());
    }
};

TEST_P(DeleteRecoveryWindowTest, ReopenedDeleteIntentSettlesAndRebuildsAuditedMetadataCapacity) {
    record_committed();
    const auto original = artifacts_->get_artifact({7}).value();
    {
        auto recovery =
            rodaq::task::TaskRecoveryStore::open(root_ / "recovery", original.summary.task_id);
        ASSERT_TRUE(recovery.ok());
        auto record = recovery.value()->load();
        ASSERT_TRUE(record.ok());
        const auto expected = record.value().record_revision++;
        record.value().committed_recording_end_ns = original.summary.recording_range.end_ns;
        ASSERT_TRUE(recovery.value()->publish(record.value(), expected).ok());
    }
    *delete_failed_step_ = static_cast<int>(GetParam());
    EXPECT_FALSE(catalog_
                     ->delete_artifact(
                         {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                         std::chrono::steady_clock::now() + 1s)
                     .ok());
    ASSERT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
    *delete_failed_step_ = -1;
    reopen_ledger_without_runtime_caches();
    ASSERT_TRUE(
        artifact::recover_deleting_artifact(volume_, *ledger_, 7, options_.manifest_limits).ok());
    const auto deleted = ledger_->get(7).value();
    EXPECT_EQ(deleted.state, artifact::LedgerState::Deleted);
    EXPECT_EQ(deleted.delete_generation, 1);
    EXPECT_EQ(deleted.delete_reason, "retention");
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7"));
    const auto usage =
        artifact::inspect_committed_volume(volume_, *ledger_, options_.manifest_limits);
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage.value().physical_bytes, 0);
    EXPECT_EQ(usage.value().artifact_slots, 1);
    artifact::CatalogOptions recovered;
    recovered.volume = volume_;
    recovered.ledger = ledger_;
    recovered.max_artifacts = 8;
    recovered.manifest = options_.manifest_limits;
    recovered.capacity = std::make_shared<artifact::VolumeCapacity>(
        std::move(artifact::VolumeCapacity::create(capacity_->policy().value(), 0, 1).value()));
    auto catalog = artifact::ArtifactCatalog::create(recovered);
    ASSERT_TRUE(catalog.ok());
    ASSERT_TRUE(catalog.value()->rebuild().ok());
    EXPECT_TRUE(catalog.value()->query({}).value().items.empty());
    EXPECT_EQ(catalog.value()->acquire({7}, {1}).status().code(), StatusCode::NotFound);
    EXPECT_EQ(recovered.capacity->snapshot().actual_bytes, 0);
    EXPECT_EQ(recovered.capacity->snapshot().artifact_slots, 1);
    auto checkpoint =
        rodaq::task::TaskRecoveryStore::open(root_ / "recovery", original.summary.task_id);
    ASSERT_TRUE(checkpoint.ok());
    const auto floor = checkpoint.value()->load().value().committed_recording_end_ns;
    ASSERT_TRUE(floor);
    ASSERT_TRUE(deleted.deleted_at);
    EXPECT_FALSE(
        catalog.value()
            ->collect_deleted(original.summary.task_id, *floor, *deleted.deleted_at + 1s, 2s)
            .value());
    EXPECT_EQ(recovered.capacity->snapshot().artifact_slots, 1);
    EXPECT_TRUE(
        catalog.value()
            ->collect_deleted(original.summary.task_id, *floor, *deleted.deleted_at + 3s, 2s)
            .value());
    EXPECT_EQ(recovered.capacity->snapshot().artifact_slots, 0);
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
    EXPECT_EQ(checkpoint.value()->load().value().committed_recording_end_ns, floor);
}

INSTANTIATE_TEST_SUITE_P(
    PartialDeletionWindows, DeleteRecoveryWindowTest,
    testing::Values(
        artifact::DeleteStep::UnlinkFile, artifact::DeleteStep::UnlinkManifest,
        artifact::DeleteStep::RemoveDirectory, artifact::DeleteStep::SyncParent));

TEST_F(PipelineControllerTest, DeleteRecoveryWithoutManifestCannotEraseAnUnknownRemainingFile) {
    record_committed();
    *delete_failed_step_ = static_cast<int>(artifact::DeleteStep::RemoveDirectory);
    ASSERT_FALSE(catalog_
                     ->delete_artifact(
                         {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                         std::chrono::steady_clock::now() + 1s)
                     .ok());
    *delete_failed_step_ = -1;
    const auto directory = volume_->artifacts_directory() / "7";
    ASSERT_FALSE(fs::exists(directory / "manifest.json"));
    std::ofstream(directory / "foreign") << "retain";
    EXPECT_EQ(
        artifact::recover_deleting_artifact(volume_, *ledger_, 7, options_.manifest_limits)
            .status()
            .code(),
        StatusCode::DataLoss);
    EXPECT_EQ(rodaq::persistence::read_file(directory, "foreign", 16).value(), "retain");
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
}

TEST_F(
    PipelineControllerTest, DeletedRecoveryNeverUsesTombstoneAsPermissionToEraseReappearedFiles) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    const auto directory = volume_->artifacts_directory() / "7";
    ASSERT_TRUE(fs::create_directory(directory));
    std::ofstream(directory / "foreign") << "retain";
    EXPECT_EQ(
        artifact::recover_deleting_artifact(volume_, *ledger_, 7, options_.manifest_limits)
            .status()
            .code(),
        StatusCode::DataLoss);
    EXPECT_EQ(rodaq::persistence::read_file(directory, "foreign", 16).value(), "retain");
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
}

class DeleteRecoveryFaultTest : public PipelineControllerTest,
                                public testing::WithParamInterface<artifact::DeleteStep> {};

TEST_P(
    DeleteRecoveryFaultTest, RecoveryFailureKeepsDeleteIntentAndAFreshAttemptSettlesActualEffects) {
    record_committed();
    *delete_failed_step_ = static_cast<int>(artifact::DeleteStep::UnlinkFile);
    ASSERT_FALSE(catalog_
                     ->delete_artifact(
                         {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                         std::chrono::steady_clock::now() + 1s)
                     .ok());
    *delete_failed_step_ = -1;
    const auto failed = artifact::recover_deleting_artifact(
        volume_, *ledger_, 7, options_.manifest_limits,
        [&](auto step) { return step == GetParam() ? EIO : 0; });
    EXPECT_FALSE(failed.ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
    ASSERT_TRUE(
        artifact::recover_deleting_artifact(volume_, *ledger_, 7, options_.manifest_limits).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7"));
}

INSTANTIATE_TEST_SUITE_P(
    RecoveryIoSteps, DeleteRecoveryFaultTest,
    testing::Values(
        artifact::DeleteStep::UnlinkFile, artifact::DeleteStep::SyncDirectory,
        artifact::DeleteStep::UnlinkManifest, artifact::DeleteStep::RemoveDirectory,
        artifact::DeleteStep::SyncParent));

TEST_F(PipelineControllerTest, DeleteRecoveryReconcilesUncertainDeletedPublicationWithoutRollback) {
    record_committed();
    *delete_failed_step_ = static_cast<int>(artifact::DeleteStep::UnlinkFile);
    ASSERT_FALSE(catalog_
                     ->delete_artifact(
                         {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                         std::chrono::steady_clock::now() + 1s)
                     .ok());
    *delete_failed_step_ = -1;
    *ledger_fault_ = true;
    EXPECT_FALSE(
        artifact::recover_deleting_artifact(volume_, *ledger_, 7, options_.manifest_limits).ok());
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7"));
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::Unavailable);
    *ledger_fault_ = false;
    ASSERT_TRUE(
        artifact::recover_deleting_artifact(volume_, *ledger_, 7, options_.manifest_limits).ok());
    const auto deleted = ledger_->get(7).value();
    EXPECT_EQ(deleted.state, artifact::LedgerState::Deleted);
    EXPECT_EQ(deleted.delete_generation, 1);
    EXPECT_EQ(deleted.delete_reason, "retention");
    ASSERT_TRUE(
        artifact::recover_deleting_artifact(volume_, *ledger_, 7, options_.manifest_limits).ok());
    EXPECT_EQ(ledger_->get(7).value().deleted_at, deleted.deleted_at);
}

TEST_F(
    PipelineControllerTest, DeleteRecoveryRechecksNamesAfterParentDurabilityBeforeLedgerDeleted) {
    record_committed();
    *delete_failed_step_ = static_cast<int>(artifact::DeleteStep::UnlinkFile);
    ASSERT_FALSE(catalog_
                     ->delete_artifact(
                         {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                         std::chrono::steady_clock::now() + 1s)
                     .ok());
    *delete_failed_step_ = -1;
    const auto directory = volume_->artifacts_directory() / "7";
    unsigned parents = 0;
    const auto result = artifact::recover_deleting_artifact(
        volume_, *ledger_, 7, options_.manifest_limits, [&](auto step) {
            if (step == artifact::DeleteStep::SyncParent && ++parents == 2) {
                EXPECT_TRUE(fs::create_directory(directory));
                std::ofstream(directory / "foreign") << "retain";
            }
            return 0;
        });
    EXPECT_EQ(result.status().code(), StatusCode::DataLoss);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
    EXPECT_EQ(rodaq::persistence::read_file(directory, "foreign", 16).value(), "retain");
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

TEST_F(PipelineControllerTest, ArtifactDeletionWaitsForReadLeaseAndRetainsTombstoneCapacity) {
    record_committed();
    const auto info = artifacts_->get_artifact({7}).value();
    const auto before = capacity_->snapshot();
    {
        auto reader = artifacts_->acquire_read({7}, info.files[0].file_id);
        ASSERT_TRUE(reader.ok());
        EXPECT_EQ(
            catalog_
                ->delete_artifact(
                    {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                    std::chrono::steady_clock::now() + 1ms)
                .status()
                .code(),
            StatusCode::DeadlineExceeded);
        EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
        EXPECT_FALSE(artifacts_->acquire_read({7}, info.files[0].file_id).ok());
        std::array<std::byte, 1> bytes{};
        EXPECT_TRUE(reader.value().read_at(0, {bytes.data(), bytes.size()}).ok());
        EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / "7"));
    }
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    const auto deleted = ledger_->get(7);
    ASSERT_TRUE(deleted.ok());
    EXPECT_EQ(deleted.value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(deleted.value().deleted_at, rodaq::WallTime{1s});
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7"));
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
    EXPECT_EQ(artifacts_->get_artifact({7}).status().code(), StatusCode::NotFound);
    EXPECT_EQ(capacity_->snapshot().actual_bytes, 0);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, before.artifact_slots);
    EXPECT_EQ(catalog_->snapshot().pending, 0);
    EXPECT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{2s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    EXPECT_EQ(ledger_->get(7).value().deleted_at, rodaq::WallTime{1s});
}

class ArtifactDeleteFailureTest : public PipelineControllerTest,
                                  public testing::WithParamInterface<artifact::DeleteStep> {};

TEST_P(ArtifactDeleteFailureTest, ActualFilesystemFailureRetainsIntentAndChargeUntilRetry) {
    record_committed();
    const auto before = capacity_->snapshot();
    *delete_failed_step_ = static_cast<int>(GetParam());
    const auto failed = catalog_->delete_artifact(
        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
        std::chrono::steady_clock::now() + 1s);
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.status().code(), StatusCode::Unavailable);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
    EXPECT_EQ(capacity_->snapshot().actual_bytes, before.actual_bytes);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, before.artifact_slots);
    EXPECT_EQ(
        catalog_
            ->delete_artifact(
                {7}, 2, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                std::chrono::steady_clock::now() + 1s)
            .status()
            .code(),
        StatusCode::Conflict);
    *delete_failed_step_ = -1;
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{2s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(capacity_->snapshot().actual_bytes, 0);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, before.artifact_slots);
    EXPECT_FALSE(fs::exists(volume_->artifacts_directory() / "7"));
}

INSTANTIATE_TEST_SUITE_P(
    DeleteFilesystemBoundaries, ArtifactDeleteFailureTest,
    testing::Values(
        artifact::DeleteStep::UnlinkFile, artifact::DeleteStep::SyncDirectory,
        artifact::DeleteStep::UnlinkManifest, artifact::DeleteStep::RemoveDirectory,
        artifact::DeleteStep::SyncParent));

TEST_F(PipelineControllerTest, ArtifactDeletionDoesNotRelabelMissingHistoryAsLegalDeletion) {
    record_committed();
    const auto before = capacity_->snapshot();
    fs::remove_all(volume_->artifacts_directory() / "7");
    const auto deleted = catalog_->delete_artifact(
        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
        std::chrono::steady_clock::now() + 1s);
    EXPECT_EQ(deleted.status().code(), StatusCode::DataLoss);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
    EXPECT_EQ(capacity_->snapshot().actual_bytes, before.actual_bytes);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, before.artifact_slots);
}

TEST_F(PipelineControllerTest, ArtifactDeleteIntentDoesNotAuthorizeUnknownFileRemoval) {
    record_committed();
    *delete_failed_step_ = static_cast<int>(artifact::DeleteStep::UnlinkFile);
    ASSERT_FALSE(catalog_
                     ->delete_artifact(
                         {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                         std::chrono::steady_clock::now() + 1s)
                     .ok());
    const auto foreign = volume_->artifacts_directory() / "7/foreign";
    std::ofstream(foreign) << "foreign";
    *delete_failed_step_ = -1;
    const auto retried = catalog_->delete_artifact(
        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
        std::chrono::steady_clock::now() + 1s);
    EXPECT_EQ(retried.status().code(), StatusCode::DataLoss);
    EXPECT_TRUE(fs::exists(foreign));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
    EXPECT_GT(capacity_->snapshot().actual_bytes, 0);
    EXPECT_EQ(capacity_->snapshot().protection, StatusCode::DataLoss);
}

class ArtifactDeletePublicationTest : public PipelineControllerTest,
                                      public testing::WithParamInterface<bool> {};

TEST_P(ArtifactDeletePublicationTest, UncertainPublicationMustReconcileBeforeReleasingCharge) {
    record_committed();
    const auto before = capacity_->snapshot();
    if (GetParam()) {
        *fail_deleted_publication_ = true;
    } else {
        *ledger_fault_ = true;
    }
    const auto first = catalog_->delete_artifact(
        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
        std::chrono::steady_clock::now() + 1s);
    ASSERT_FALSE(first.ok());
    EXPECT_EQ(first.status().code(), StatusCode::Unavailable);
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(fs::exists(volume_->artifacts_directory() / "7"), !GetParam());
    EXPECT_EQ(capacity_->snapshot().actual_bytes, before.actual_bytes);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, before.artifact_slots);
    *ledger_fault_ = false;
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{2s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(capacity_->snapshot().actual_bytes, 0);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, before.artifact_slots);
    EXPECT_EQ(catalog_->snapshot().pending, 0);
}

INSTANTIATE_TEST_SUITE_P(
    DeletePublicationBoundaries, ArtifactDeletePublicationTest, testing::Bool());

TEST_F(PipelineControllerTest, DeletedArtifactReappearanceIsProtectedWithoutDeletingForeignData) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now() + 1s)
                    .ok());
    ASSERT_TRUE(fs::create_directory(volume_->artifacts_directory() / "7"));
    const auto foreign = volume_->artifacts_directory() / "7/foreign";
    std::ofstream(foreign) << "foreign";
    const auto retried = catalog_->delete_artifact(
        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{2s},
        std::chrono::steady_clock::now() + 1s);
    EXPECT_EQ(retried.status().code(), StatusCode::DataLoss);
    EXPECT_TRUE(fs::exists(foreign));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(capacity_->snapshot().protection, StatusCode::DataLoss);
    EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
}

TEST_F(PipelineControllerTest, ActualDeletionRetainsOneOwnerWhileParentSyncObservationTimesOut) {
    record_committed();
    const auto before = capacity_->snapshot();
    delete_control_->block = true;
    auto deleting = std::async(std::launch::async, [&] {
        return catalog_->delete_artifact(
            {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
            std::chrono::steady_clock::now() + 1s);
    });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(delete_control_->mutex);
        entered =
            delete_control_->changed.wait_for(lock, 2s, [&] { return delete_control_->entered; });
    }
    if (entered) {
        EXPECT_EQ(deleting.wait_for(1ms), std::future_status::timeout);
        EXPECT_EQ(catalog_->snapshot().delete_inflight, 1);
        EXPECT_EQ(catalog_->snapshot().commit_inflight, 0);
        EXPECT_TRUE(artifacts_->query_artifacts({}).value().items.empty());
        EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleting);
        EXPECT_EQ(capacity_->snapshot().actual_bytes, before.actual_bytes);
        EXPECT_EQ(
            catalog_
                ->delete_artifact(
                    {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                    std::chrono::steady_clock::now() + 1ms)
                .status()
                .code(),
            StatusCode::Conflict);
    }
    {
        std::lock_guard<std::mutex> lock(delete_control_->mutex);
        delete_control_->released = true;
    }
    delete_control_->changed.notify_all();
    const auto completed = deleting.get();
    ASSERT_TRUE(entered);
    ASSERT_TRUE(completed.ok());
    EXPECT_EQ(catalog_->snapshot().delete_inflight, 0);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(capacity_->snapshot().actual_bytes, 0);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, before.artifact_slots);
}

TEST_F(PipelineControllerTest, RetentionUsesTaskWideCommittedTimeAndPreservesTheLeftBoundary) {
    retention_monotonic = 200;
    options_.receive_clock = &retention_clock;
    create();
    second(false);
    ASSERT_TRUE(controller_);
    ASSERT_TRUE(second_);
    auto control = accepted_run();
    const auto selected = rodaq::task::RunAuthorization::create(
        control, rodaq::StartPolicyMode::AllRequired,
        {{controller_, options_.manifest.effective_config_id, true},
         {second_, options_.manifest.effective_config_id, true}},
        recovery(), boot(), &retention_clock);
    ASSERT_TRUE(selected.ok());
    const auto prepared = selected.value()->prepare(soon());
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(prepared.value().barrier_passed);
    ASSERT_TRUE(selected.value()->publish_authorization(0).ok());
    ASSERT_TRUE(selected.value()->open(control, 200).ok());
    const auto wait_receive = [&](const auto& pipeline, std::int64_t realtime) {
        const auto before = pipeline->snapshot().value().gate.accepted_records;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            const auto facts = pipeline->callback_facts();
            const auto admitted = pipeline->snapshot();
            if (facts.ok() && admitted.ok() &&
                admitted.value().gate.accepted_records >= before + 4 &&
                facts.value().streams[0].last_receive_realtime_ns == realtime) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    retention_monotonic = 500;
    ASSERT_TRUE(wait_receive(controller_, 1400));
    ASSERT_TRUE(controller_->establish_control_fence().ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    auto first = controller_->drain(soon());
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value());
    commit(std::move(*first.value()));
    ASSERT_TRUE(controller_->quiesce(soon()).ok());
    retention_monotonic = 1200;
    ASSERT_TRUE(wait_receive(second_, 2100));
    ASSERT_TRUE(second_->establish_control_fence().ok());
    ASSERT_TRUE(second_->close_admission(8).ok());
    auto later = second_->drain(soon());
    ASSERT_TRUE(later.ok());
    ASSERT_TRUE(later.value());
    commit(std::move(*later.value()));
    ASSERT_TRUE(second_->quiesce(soon()).ok());
    const auto old = artifacts_->get_artifact({7}).value().summary;
    const auto recent = artifacts_->get_artifact({8}).value().summary;
    ASSERT_EQ(old.recording_range.end_ns, 300);
    ASSERT_EQ(recent.recording_range.end_ns, 1000);
    const std::vector<std::string> versions{options_.manifest.config_version};
    rodaq::RetentionPolicy policy{rodaq::RetentionMode::Rolling, 700ns, {}};
    auto choice = catalog_->select_retention(control.task_id, policy, versions);
    ASSERT_TRUE(choice.ok());
    EXPECT_EQ(choice.value().committed_recording_end_ns, 1000);
    EXPECT_FALSE(choice.value().candidate);
    policy.max_duration = 699ns;
    choice = catalog_->select_retention(control.task_id, policy, versions);
    ASSERT_TRUE(choice.ok());
    ASSERT_TRUE(choice.value().candidate);
    EXPECT_EQ(choice.value().candidate->value, 7);
    const auto excluded = catalog_->select_retention(control.task_id, policy, {});
    ASSERT_TRUE(excluded.ok());
    EXPECT_EQ(excluded.value().committed_recording_end_ns, 1000);
    EXPECT_EQ(excluded.value().retained_physical_bytes, 0);
    EXPECT_FALSE(excluded.value().candidate);
    auto other = control.task_id;
    other.value[0] ^= 1;
    EXPECT_FALSE(catalog_->select_retention(other, policy, versions).value().candidate);
    // Advancing host time without a new durable commit never moves the window.
    retention_monotonic = 100000;
    EXPECT_EQ(
        catalog_->select_retention(control.task_id, policy, versions)
            .value()
            .committed_recording_end_ns,
        1000);
}

TEST_F(
    PipelineControllerTest, RetentionByteTargetCannotOverrideDurationProtectionOrActiveReadLease) {
    record_committed();
    const std::vector<std::string> versions{options_.manifest.config_version};
    rodaq::RetentionPolicy policy{rodaq::RetentionMode::Rolling, 1ns, 1};
    auto choice = catalog_->select_retention(options_.identity.task_id, policy, versions);
    ASSERT_TRUE(choice.ok());
    EXPECT_FALSE(choice.value().byte_target_satisfied);
    EXPECT_FALSE(choice.value().candidate);
    policy.max_duration.reset();
    {
        const auto info = artifacts_->get_artifact({7}).value();
        auto reader = artifacts_->acquire_read({7}, info.files[0].file_id);
        ASSERT_TRUE(reader.ok());
        choice = catalog_->select_retention(options_.identity.task_id, policy, versions);
        ASSERT_TRUE(choice.ok());
        EXPECT_FALSE(choice.value().candidate);
        EXPECT_GT(choice.value().retained_physical_bytes, 1);
    }
    choice = catalog_->select_retention(options_.identity.task_id, policy, versions);
    ASSERT_TRUE(choice.ok());
    ASSERT_TRUE(choice.value().candidate);
    EXPECT_EQ(choice.value().candidate->value, 7);
    policy = {};
    EXPECT_FALSE(
        catalog_->select_retention(options_.identity.task_id, policy, versions).value().candidate);
}

class AutomaticRetentionTest : public PipelineControllerTest,
                               public testing::WithParamInterface<bool> {};

TEST_P(AutomaticRetentionTest, ResolvesContinuousHistoryFromItsImmutableConfig) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 1);
    const auto previous = artifacts.value().items[0];
    config = service.get_active_config(id).value();
    config.recording_mode = rodaq::RecordingMode::Rolling;
    config.retention_policy = {rodaq::RetentionMode::Rolling, {}, 1, GetParam()};
    ASSERT_TRUE(composition_->observe().ok());
    const auto configure = service.configure(id, config, {"rolling"}, {true, 3});
    ASSERT_TRUE(configure.ok());
    ASSERT_TRUE(wait_composed(id, configure.value()));
    ASSERT_TRUE(composition_->observe().ok());
    const auto ledger = ledger_->get(previous.segment_id);
    ASSERT_TRUE(ledger.ok());
    EXPECT_EQ(
        ledger.value().state,
        GetParam() ? artifact::LedgerState::Deleted : artifact::LedgerState::Committed);
    artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    EXPECT_EQ(artifacts.value().items.size(), GetParam() ? 0 : 1);
    // New Rolling artifacts participate even when old Continuous history is excluded.
    const auto rolling_start = service.start(id, {"rolling-start"}, {true, 4});
    ASSERT_TRUE(rolling_start.ok());
    ASSERT_TRUE(wait_composed(id, rolling_start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto rolling_stop = service.stop(id, {"rolling-stop"}, {true, 5});
    ASSERT_TRUE(rolling_stop.ok());
    ASSERT_TRUE(wait_composed(id, rolling_stop.value()));
    artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    const auto rolling = std::find_if(
        artifacts.value().items.begin(), artifacts.value().items.end(),
        [&](const auto& row) { return !(row.artifact_id == previous.artifact_id); });
    ASSERT_NE(rolling, artifacts.value().items.end());
    const auto info = composition_->artifacts().get_artifact(rolling->artifact_id).value();
    {
        auto reader =
            composition_->artifacts().acquire_read(rolling->artifact_id, info.files[0].file_id);
        ASSERT_TRUE(reader.ok());
        ASSERT_TRUE(composition_->observe().ok());
        EXPECT_EQ(
            ledger_->get(rolling->segment_id).value().state, artifact::LedgerState::Committed);
    }
    ASSERT_TRUE(composition_->observe().ok());
    EXPECT_EQ(ledger_->get(rolling->segment_id).value().state, artifact::LedgerState::Deleted);
    ASSERT_TRUE(composition_->observe().ok());
    EXPECT_EQ(service.get_task(id).value().control_generation, 6);
}

INSTANTIATE_TEST_SUITE_P(ContinuousHistory, AutomaticRetentionTest, testing::Bool());

TEST_F(PipelineControllerTest, AutomaticRetentionRetainsUncertainIntentAndOrdersConfigure) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recording_mode = rodaq::RecordingMode::Rolling;
    config.retention_policy = {rodaq::RetentionMode::Rolling, {}, 1};
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    const auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 1);
    const auto artifact = artifacts.value().items[0];
    *ledger_fault_ = true;
    const auto uncertain = composition_->observe();
    EXPECT_FALSE(uncertain.ok());
    EXPECT_TRUE(fs::exists(volume_->artifacts_directory() / std::to_string(artifact.segment_id)));
    config = service.get_active_config(id).value();
    config.recording_mode = rodaq::RecordingMode::Continuous;
    config.retention_policy = {};
    const auto conflict = service.configure(id, config, {"keep"}, {true, 3});
    EXPECT_FALSE(conflict.ok());
    *ledger_fault_ = false;
    ASSERT_TRUE(composition_->observe().ok());
    const auto deleted = ledger_->get(artifact.segment_id);
    ASSERT_TRUE(deleted.ok());
    EXPECT_EQ(deleted.value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(deleted.value().delete_generation, 3);
    const auto configure = service.configure(id, config, {"keep"}, {true, 3});
    ASSERT_TRUE(configure.ok());
    ASSERT_TRUE(wait_composed(id, configure.value()));
    EXPECT_EQ(service.get_task(id).value().control_generation, 4);
}

TEST_F(PipelineControllerTest, ShutdownCannotDeclareActualRetentionPublicationSettled) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recording_mode = rodaq::RecordingMode::Rolling;
    config.retention_policy = {rodaq::RetentionMode::Rolling, {}, 1};
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    ledger_control_->block = true;
    auto maintenance = std::async(std::launch::async, [&] { return composition_->observe(); });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(ledger_control_->mutex);
        entered =
            ledger_control_->changed.wait_for(lock, 2s, [&] { return ledger_control_->entered; });
    }
    const auto shutdown = composition_->shutdown(std::chrono::steady_clock::now() + 10ms);
    {
        std::lock_guard<std::mutex> lock(ledger_control_->mutex);
        ledger_control_->released = true;
    }
    ledger_control_->changed.notify_all();
    const auto actual = maintenance.get();
    ASSERT_TRUE(entered);
    EXPECT_FALSE(shutdown.ok());
    EXPECT_TRUE(actual.ok());
    EXPECT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
}

TEST_F(PipelineControllerTest, CommittedProgressSurvivesDeletionOfItsNewestArtifact) {
    retention_monotonic = 200;
    open_composition(&retention_clock);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    const auto id = service.create_task(config, {"create"}).value();
    const auto record_run = [&](const char* key, std::uint64_t generation, std::uint64_t begin,
                                std::uint64_t end) {
        retention_monotonic = begin;
        const auto start = service.start(id, {std::string(key) + "-start"}, {true, generation});
        if (!start.ok() || !wait_composed(id, start.value()) || !composition_->observe().ok()) {
            return false;
        }
        const auto before =
            service.get_runtime_snapshot(id).value().pipelines[0].facts->accepted_records;
        retention_monotonic = end;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool accepted = false;
        do {
            if (!composition_->observe().ok()) {
                return false;
            }
            const auto runtime = service.get_runtime_snapshot(id);
            accepted = runtime.ok() && runtime.value().pipelines[0].facts &&
                       runtime.value().pipelines[0].facts->accepted_records >= before + 4;
            if (accepted) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        const auto stop = service.stop(id, {std::string(key) + "-stop"}, {true, generation + 1});
        return accepted && stop.ok() && wait_composed(id, stop.value());
    };
    ASSERT_TRUE(record_run("continuous", 1, 200, 500));
    ASSERT_TRUE(composition_->observe().ok());
    config = service.get_active_config(id).value();
    config.recording_mode = rodaq::RecordingMode::Rolling;
    config.retention_policy = {rodaq::RetentionMode::Rolling, {}, 1};
    const auto rolling = service.configure(id, config, {"rolling"}, {true, 3});
    ASSERT_TRUE(rolling.ok());
    ASSERT_TRUE(wait_composed(id, rolling.value()));
    ASSERT_TRUE(record_run("rolling", 4, 1000, 1300));
    ASSERT_TRUE(composition_->observe().ok());
    auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 1);
    EXPECT_EQ(artifacts.value().items[0].recording_range.end_ns, 300);
    config = service.get_active_config(id).value();
    config.retention_policy = {rodaq::RetentionMode::Rolling, 100ns, {}, true};
    const auto include = service.configure(id, config, {"include"}, {true, 6});
    ASSERT_TRUE(include.ok());
    ASSERT_TRUE(wait_composed(id, include.value()));
    retention_monotonic = 10000;
    ASSERT_TRUE(composition_->observe().ok());
    artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    EXPECT_TRUE(artifacts.value().items.empty());
    const auto bytes = rodaq::persistence::read_file(
        root_ / "service/recovery", rodaq::to_string(id) + ".json", 262144);
    ASSERT_TRUE(bytes.ok());
    const auto checkpoint = rodaq::task::decode_recovery_record(bytes.value(), {});
    ASSERT_TRUE(checkpoint.ok());
    EXPECT_EQ(checkpoint.value().committed_recording_end_ns, 600);
    EXPECT_EQ(checkpoint.value().recording_time_ns, 600);
}

TEST_F(PipelineControllerTest, RunAuthorizationPreservesInterleavedCommittedCheckpoint) {
    create();
    auto control = accepted_run();
    auto store = recovery();
    auto run = rodaq::task::RunAuthorization::create(
        control, rodaq::StartPolicyMode::AllRequired,
        {{controller_, options_.manifest.effective_config_id, true}}, store, boot(),
        &receive_clock);
    ASSERT_TRUE(run.ok());
    ASSERT_TRUE(run.value()->prepare(soon()).value().barrier_passed);
    ASSERT_TRUE(run.value()->checkpoint_prepared().ok());
    auto checkpoint = store->load().value();
    const auto expected = checkpoint.record_revision++;
    checkpoint.committed_recording_end_ns = 123;
    ASSERT_TRUE(store->publish(checkpoint, expected).ok());
    ASSERT_TRUE(run.value()->publish_authorization(0).ok());
    EXPECT_EQ(store->load().value().committed_recording_end_ns, 123);
    ASSERT_TRUE(run.value()->fence().ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    ASSERT_TRUE(controller_->quiesce(soon()).ok());
}

TEST_F(PipelineControllerTest, LowVolumeRejectsNewPreparationBeforeAnyNativeResourcesAreCreated) {
    auto probe = capacity_->begin_space_sample(200).value();
    ASSERT_TRUE(capacity_->finish_space_sample(probe, 150, 200).ok());
    create();
    const auto prepared = controller_->prepare(soon());
    EXPECT_EQ(prepared.status().code(), StatusCode::ResourceExhausted);
    EXPECT_FALSE(controller_->snapshot().value().prepared);
    EXPECT_TRUE(ledger_->recovery_snapshot().value().empty());
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
    EXPECT_EQ(capacity_->snapshot().segment_slots, 0);
    ASSERT_TRUE(controller_->establish_control_fence(200).ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    EXPECT_TRUE(controller_->quiesce(soon()).ok());
}

TEST_F(PipelineControllerTest, NewGateRevalidatesLowWatermarkAfterActualPreparation) {
    create();
    const auto prepared = controller_->prepare(soon());
    ASSERT_TRUE(prepared.ok());
    auto authorization = authorize(prepared.value());
    auto probe = capacity_->begin_space_sample(200).value();
    const auto promised = capacity_->snapshot().future_bytes;
    ASSERT_TRUE(capacity_->finish_space_sample(probe, promised + 150, 200).ok());
    EXPECT_EQ(controller_->open_admission(authorization), pipeline::GateOpenResult::Obsolete);
    EXPECT_FALSE(controller_->snapshot().value().gate.ever_opened);
    ASSERT_TRUE(controller_->establish_control_fence(200).ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    EXPECT_TRUE(controller_->quiesce(soon()).ok());
}

class VolumeAdmissionProtectionTest : public PipelineControllerTest,
                                      public testing::WithParamInterface<bool> {};

TEST_P(VolumeAdmissionProtectionTest, LocalCapacityFailureFencesWithoutLosingCoveredAcceptedData) {
    create();
    const auto prepared = controller_->prepare(soon());
    ASSERT_TRUE(prepared.ok());
    ASSERT_EQ(
        controller_->open_admission(authorize(prepared.value())), pipeline::GateOpenResult::Opened);
    ASSERT_TRUE(wait_written(4));
    if (GetParam()) {
        capacity_->invalidate_space_sample();
    } else {
        auto probe = capacity_->begin_space_sample(200).value();
        ASSERT_TRUE(capacity_->finish_space_sample(probe, 0, 200).ok());
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool fenced = false;
    do {
        const auto snapshot = controller_->snapshot();
        fenced = snapshot.ok() && snapshot.value().gate.control_fence;
        if (fenced) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(fenced);
    EXPECT_NE(controller_->snapshot().value().capacity.admission_failure, StatusCode::Ok);
    EXPECT_EQ(controller_->snapshot().value().capacity.protection, StatusCode::Ok);
    ASSERT_TRUE(controller_->close_admission(8).ok());
    const auto accepted = controller_->snapshot().value().gate.accepted_records;
    auto sealed = controller_->drain(soon());
    ASSERT_TRUE(sealed.ok());
    ASSERT_TRUE(sealed.value());
    const auto metadata = sealed.value()->segment->metadata.snapshot();
    EXPECT_EQ(metadata.accepted, accepted);
    EXPECT_EQ(metadata.written, accepted);
    EXPECT_EQ(metadata.known_failed, 0);
    EXPECT_EQ(metadata.unknown, 0);
    commit(std::move(*sealed.value()));
    ASSERT_TRUE(controller_->quiesce(soon()).ok());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Committed);
}

INSTANTIATE_TEST_SUITE_P(StopOrUnknownSpace, VolumeAdmissionProtectionTest, testing::Bool());

class TaskVolumeProtectionTest : public PipelineControllerTest,
                                 public testing::WithParamInterface<bool> {};

TEST_P(TaskVolumeProtectionTest, MaintenanceDrainsProtectedRunWithoutInventingAUserOperation) {
    open_composition();
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    if (GetParam()) {
        config.pipelines[0].acquisition.json = R"({"streams":["first","second"],"mode":"silent"})";
    }
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    if (!GetParam()) {
        ASSERT_TRUE(wait_composed_records(id));
    }
    const auto operations_before = service.list_operations(id, {}).value().items.size();
    auto probe = capacity_->begin_space_sample(200).value();
    ASSERT_TRUE(capacity_->finish_space_sample(probe, 0, 200).ok());
    ASSERT_TRUE(composition_->observe().ok());
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    const auto current = service.get_task(id).value();
    EXPECT_EQ(current.desired_state, rodaq::DesiredState::Running);
    EXPECT_EQ(current.control_generation, 2);
    EXPECT_EQ(
        service.get_operation(id, start.value()).value().state, rodaq::OperationState::Succeeded);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), operations_before);
    const auto runtime = service.get_runtime_snapshot(id).value();
    ASSERT_TRUE(runtime.pipelines[0].facts);
    EXPECT_FALSE(runtime.pipelines[0].facts->admission_open);
    EXPECT_TRUE(runtime.pipelines[0].facts->quiescent);
    EXPECT_EQ(
        runtime.pipelines[0].facts->accepted_records, runtime.pipelines[0].facts->written_records);
    const auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    EXPECT_EQ(artifacts.value().items.size(), GetParam() ? 0 : 1);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(service.get_task(id).value().desired_state, rodaq::DesiredState::Stopped);
}

INSTANTIATE_TEST_SUITE_P(ActiveOrSilentSource, TaskVolumeProtectionTest, testing::Bool());

class AutomaticVolumeRecoveryTest : public PipelineControllerTest,
                                    public testing::WithParamInterface<bool> {};

TEST_P(
    AutomaticVolumeRecoveryTest, ReauthorizesNewInstancesWithoutChangingUserOperationOrTaskTime) {
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, GetParam() ? 2 : 1, 24h, {3, 10ns, 40ns});
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recovery_policy = rodaq::RecoveryPolicyMode::Resume;
    if (GetParam()) {
        config.pipelines.push_back(config.pipelines.front());
    }
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    protect_composed_volume(500);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    const auto previous = composed_recovery(id);
    ASSERT_TRUE(previous.ok());
    ASSERT_TRUE(previous.value().run_operation);
    EXPECT_EQ(previous.value().run_operation->phase, rodaq::task::RunPhase::Ended);
    EXPECT_EQ(previous.value().recording_time_ns, 300);
    retention_monotonic = 510;
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Active));
    const auto recovered = composed_recovery(id);
    ASSERT_TRUE(recovered.ok());
    ASSERT_TRUE(recovered.value().run_operation);
    EXPECT_EQ(recovered.value().run_operation->phase, rodaq::task::RunPhase::OpenAuthorized);
    EXPECT_EQ(recovered.value().run_operation->operation_id, start.value());
    EXPECT_EQ(recovered.value().run_operation->control_generation, 2);
    EXPECT_EQ(
        recovered.value().pipelines.size(),
        previous.value().run_operation->authorized_pipelines.size());
    for (const auto& recent : recovered.value().pipelines) {
        EXPECT_TRUE(recent.instance.ended_boundary);
    }
    for (const auto& instance : recovered.value().run_operation->authorized_pipelines) {
        EXPECT_TRUE(std::none_of(
            previous.value().run_operation->authorized_pipelines.begin(),
            previous.value().run_operation->authorized_pipelines.end(),
            [&](const auto& old) { return old.instance_id == instance.instance_id; }));
    }
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
    EXPECT_EQ(
        service.get_operation(id, start.value()).value().state, rodaq::OperationState::Succeeded);
    ASSERT_TRUE(wait_composed_receive(id, 810));
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(composed_recovery(id).value().recording_time_ns, 600);
    const auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    EXPECT_EQ(artifacts.value().items.size(), GetParam() ? 4 : 2);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
}

INSTANTIATE_TEST_SUITE_P(OneOrTwoPipelines, AutomaticVolumeRecoveryTest, testing::Bool());

TEST_F(PipelineControllerTest, FailedObservationStillRegistersRecoveryBeforeReturningItsError) {
    retention_monotonic = 200;
    open_composition(&recoverable_clock);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recovery_policy = rodaq::RecoveryPolicyMode::Resume;
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    const auto previous = composed_recovery(id).value().run_operation->authorized_pipelines;
    clock_failed = true;
    const auto failed = composition_->observe();
    clock_failed = false;
    ASSERT_FALSE(failed.ok());
    EXPECT_EQ(failed.status().code(), StatusCode::Unavailable);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    retention_monotonic = 510;
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Active));
    const auto recovered = composed_recovery(id).value();
    ASSERT_TRUE(recovered.run_operation);
    EXPECT_FALSE(
        recovered.run_operation->authorized_pipelines[0].instance_id == previous[0].instance_id);
    EXPECT_EQ(recovered.run_operation->operation_id, start.value());
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    EXPECT_TRUE(wait_composed(id, stop.value()));
}

TEST_F(PipelineControllerTest, ManualRecoveryKeepsEndedInstancesClosedAfterSpaceBecomesHealthy) {
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, 1, 24h, {3, 10ns, 40ns});
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    const auto id = service.create_task(formal_config(), {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    protect_composed_volume(500);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    const auto ended = composed_recovery(id).value();
    retention_monotonic = 510;
    for (unsigned i = 0; i < 8; ++i) {
        ASSERT_TRUE(composition_->observe().ok());
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(capacity_->admit_recording(510).ok());
    EXPECT_EQ(composed_recovery(id).value().record_revision, ended.record_revision);
    EXPECT_EQ(
        service.get_task(id).value().execution_condition, rodaq::ExecutionCondition::Protected);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
}

class SupersededVolumeRecoveryTest : public PipelineControllerTest,
                                     public testing::WithParamInterface<bool> {};

TEST_P(SupersededVolumeRecoveryTest, PauseOrStopDefeatsARealBlockedAuthorizationPublication) {
    auto armed = std::make_shared<std::atomic<bool>>(false);
    auto publications = std::make_shared<std::atomic<unsigned>>(0);
    auto blocking = std::make_shared<ReadControl>();
    composed_file_faults_ = [armed, publications, blocking](auto domain, auto step) {
        if (armed->load() && domain == rodaq::task::ServiceFileDomain::Recovery &&
            step == rodaq::persistence::FileStep::SyncFile && ++*publications == 3) {
            std::unique_lock<std::mutex> lock(blocking->mutex);
            blocking->entered = true;
            blocking->changed.notify_all();
            blocking->changed.wait(lock, [&] { return blocking->released; });
        }
        return 0;
    };
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, 1, 24h, {3, 10ns, 40ns});
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recovery_policy = rodaq::RecoveryPolicyMode::Resume;
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    protect_composed_volume(500);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    *armed = true;
    retention_monotonic = 510;
    auto observing = std::async(std::launch::async, [&] { return composition_->observe(); });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(blocking->mutex);
        entered = blocking->changed.wait_for(lock, 2s, [&] { return blocking->entered; });
    }
    auto superseding = std::async(std::launch::async, [&] {
        return GetParam() ? service.stop(id, {"supersede"}, {true, 2})
                          : service.pause(id, {"supersede"}, {true, 2});
    });
    bool durable = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        const auto current = service.get_task(id);
        durable = current.ok() && current.value().control_generation == 3 &&
                  current.value().desired_state ==
                      (GetParam() ? rodaq::DesiredState::Stopped : rodaq::DesiredState::Paused);
        if (durable) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    {
        std::lock_guard<std::mutex> lock(blocking->mutex);
        blocking->released = true;
    }
    blocking->changed.notify_all();
    const auto observed = observing.get();
    const auto accepted = superseding.get();
    ASSERT_TRUE(entered);
    ASSERT_TRUE(durable);
    EXPECT_TRUE(observed.ok());
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(wait_composed(id, accepted.value()));
    retention_monotonic = 1000;
    ASSERT_TRUE(composition_->observe().ok());
    const auto runtime = service.get_runtime_snapshot(id).value();
    ASSERT_TRUE(runtime.pipelines[0].facts);
    EXPECT_FALSE(runtime.pipelines[0].facts->admission_open);
    EXPECT_EQ(runtime.pipelines[0].facts->accepted_records, 0);
    EXPECT_EQ(service.get_task(id).value().control_generation, 3);
    EXPECT_EQ(
        service.get_operation(id, start.value()).value().state, rodaq::OperationState::Succeeded);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
}

INSTANTIATE_TEST_SUITE_P(PauseAndStop, SupersededVolumeRecoveryTest, testing::Bool());

TEST_F(PipelineControllerTest, VolumeRecoveryBackoffAndAttemptBudgetPreserveTheOriginalUserResult) {
    auto fail_next = std::make_shared<std::atomic<bool>>(false);
    auto failures = std::make_shared<std::atomic<unsigned>>(0);
    composed_file_faults_ = [fail_next, failures](auto domain, auto step) {
        if (domain == rodaq::task::ServiceFileDomain::Recovery &&
            step == rodaq::persistence::FileStep::SyncFile && fail_next->exchange(false)) {
            ++*failures;
            return EIO;
        }
        return 0;
    };
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, 1, 24h, {3, 10ns, 40ns});
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recovery_policy = rodaq::RecoveryPolicyMode::Resume;
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    protect_composed_volume(500);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (attempt != 0) {
            retention_monotonic = attempt == 1 ? 519 : 539;
            ASSERT_TRUE(composition_->observe().ok());
            EXPECT_EQ(failures->load(), attempt);
        }
        *fail_next = true;
        retention_monotonic = attempt == 0 ? 510 : attempt == 1 ? 520 : 540;
        ASSERT_TRUE(composition_->observe().ok());
        ASSERT_TRUE(wait_composed_condition(
            id, attempt == 2 ? rodaq::ExecutionCondition::Failed
                             : rodaq::ExecutionCondition::Protected));
        EXPECT_EQ(failures->load(), attempt + 1);
    }
    retention_monotonic = 1000;
    for (unsigned i = 0; i < 4; ++i) {
        ASSERT_TRUE(composition_->observe().ok());
    }
    EXPECT_EQ(failures->load(), 3);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
    EXPECT_EQ(
        service.get_operation(id, start.value()).value().state, rodaq::OperationState::Succeeded);
    EXPECT_EQ(service.get_task(id).value().desired_state, rodaq::DesiredState::Running);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
}

TEST_F(
    PipelineControllerTest, UncertainRecoveryAuthorizationNeverOpensBeforeReconciliationAndRetry) {
    auto armed = std::make_shared<std::atomic<bool>>(false);
    auto publications = std::make_shared<std::atomic<unsigned>>(0);
    composed_file_faults_ = [armed, publications](auto domain, auto step) {
        return armed->load() && domain == rodaq::task::ServiceFileDomain::Recovery &&
                       step == rodaq::persistence::FileStep::SyncDirectory && ++*publications == 3
                   ? EIO
                   : 0;
    };
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, 1, 24h, {3, 10ns, 40ns});
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recovery_policy = rodaq::RecoveryPolicyMode::Resume;
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    protect_composed_volume(500);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    *armed = true;
    retention_monotonic = 510;
    ASSERT_TRUE(composition_->observe().ok());
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    const auto runtime = service.get_runtime_snapshot(id).value();
    ASSERT_TRUE(runtime.pipelines[0].facts);
    EXPECT_FALSE(runtime.pipelines[0].facts->admission_open);
    EXPECT_EQ(runtime.pipelines[0].facts->accepted_records, 0);
    EXPECT_EQ(composed_recovery(id).value().run_operation->phase, rodaq::task::RunPhase::Ended);
    *armed = false;
    retention_monotonic = 520;
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Active));
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
}

TEST_F(PipelineControllerTest, QuietResourcesCannotHideAnUncertainEndedCheckpointFromMaintenance) {
    auto fail = std::make_shared<std::atomic<bool>>(false);
    composed_file_faults_ = [fail](auto domain, auto step) {
        return fail->load() && domain == rodaq::task::ServiceFileDomain::Recovery &&
                       step == rodaq::persistence::FileStep::SyncDirectory
                   ? EIO
                   : 0;
    };
    retention_monotonic = 200;
    open_composition(&retention_clock);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    const auto id = service.create_task(formal_config(), {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    *fail = true;
    auto probe = capacity_->begin_space_sample(500).value();
    ASSERT_TRUE(capacity_->finish_space_sample(probe, 0, 500).ok());
    bool pending = false;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        const auto observed = composition_->observe();
        if (!observed.ok()) {
            EXPECT_EQ(observed.status().code(), StatusCode::Unavailable);
        }
        const auto runtime = service.get_runtime_snapshot(id);
        const auto current = service.get_task(id);
        pending = runtime.ok() && runtime.value().pipelines[0].facts &&
                  runtime.value().pipelines[0].facts->quiescent && current.ok() &&
                  current.value().execution_condition == rodaq::ExecutionCondition::PendingCleanup;
        if (pending) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    *fail = false;
    ASSERT_TRUE(pending);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    const auto ended = composed_recovery(id);
    ASSERT_TRUE(ended.ok());
    ASSERT_TRUE(ended.value().run_operation);
    EXPECT_EQ(ended.value().run_operation->phase, rodaq::task::RunPhase::Ended);
    EXPECT_EQ(ended.value().recording_time_ns, 300);
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 1);
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
}

TEST_F(
    PipelineControllerTest, SilentVolumeRecoveryDoesNotWaitForFirstDataOrInventCommittedProgress) {
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, 1, 24h, {3, 10ns, 40ns});
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recovery_policy = rodaq::RecoveryPolicyMode::Resume;
    config.pipelines[0].acquisition.json = R"({"streams":["first","second"],"mode":"silent"})";
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    retention_monotonic = 500;
    protect_composed_volume(500);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Protected));
    EXPECT_EQ(composed_recovery(id).value().recording_time_ns, 300);
    retention_monotonic = 510;
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Active));
    const auto runtime = service.get_runtime_snapshot(id).value();
    ASSERT_TRUE(runtime.pipelines[0].facts);
    EXPECT_EQ(runtime.pipelines[0].facts->accepted_records, 0);
    retention_monotonic = 810;
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    const auto ended = composed_recovery(id).value();
    EXPECT_EQ(ended.recording_time_ns, 600);
    EXPECT_FALSE(ended.committed_recording_end_ns);
    EXPECT_TRUE(composition_->artifacts().query_artifacts({}).value().items.empty());
    EXPECT_EQ(service.list_operations(id, {}).value().items.size(), 2);
}

TEST_F(PipelineControllerTest, HealthySpaceCannotBypassAnActuallyBlockedOldCommitOwner) {
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, 1, 24h, {3, 10ns, 40ns});
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recovery_policy = rodaq::RecoveryPolicyMode::Resume;
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_receive(id, 500));
    const auto original = composed_recovery(id).value();
    ledger_control_->block = true;
    protect_composed_volume(500);
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(ledger_control_->mutex);
        entered =
            ledger_control_->changed.wait_for(lock, 2s, [&] { return ledger_control_->entered; });
    }
    retention_monotonic = 510;
    bool healthy = true;
    for (unsigned i = 0; i < 4; ++i) {
        healthy = composition_->observe().ok() && healthy;
    }
    const auto held = composed_recovery(id);
    const bool admitted = capacity_->admit_recording(510).ok();
    {
        std::lock_guard<std::mutex> lock(ledger_control_->mutex);
        ledger_control_->released = true;
    }
    ledger_control_->changed.notify_all();
    ASSERT_TRUE(entered);
    ASSERT_TRUE(healthy);
    EXPECT_TRUE(admitted);
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.value().record_revision, original.record_revision);
    EXPECT_EQ(
        held.value().run_operation->authorized_pipelines[0].instance_id,
        original.run_operation->authorized_pipelines[0].instance_id);
    ASSERT_TRUE(wait_composed_condition(id, rodaq::ExecutionCondition::Active));
    EXPECT_EQ(composed_recovery(id).value().recording_time_ns, 300);
    retention_monotonic = 520;
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    EXPECT_EQ(composed_recovery(id).value().recording_time_ns, 310);
}

TEST_F(PipelineControllerTest, CatalogCollectionRequiresAuditExpiryAndDurableCommittedProgress) {
    record_committed(50);
    const auto task = options_.identity.task_id;
    ASSERT_EQ(artifacts_->get_artifact({7}).value().summary.recording_range.end_ns, 50);
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now())
                    .ok());
    ASSERT_EQ(capacity_->snapshot().artifact_slots, 1);
    EXPECT_FALSE(catalog_->collect_deleted(task, 50, rodaq::WallTime{999ms}, 1s).value());
    EXPECT_FALSE(catalog_->collect_deleted(task, 50, rodaq::WallTime{1999999999ns}, 1s).value());
    EXPECT_FALSE(catalog_->collect_deleted(task, 49, rodaq::WallTime{2s}, 1s).value());
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_TRUE(catalog_->collect_deleted(task, 50, rodaq::WallTime{2s}, 1s).value());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::NotFound);
    EXPECT_FALSE(catalog_->collect_deleted(task, 50, rodaq::WallTime{3s}, 1s).value());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
}

TEST_F(PipelineControllerTest, CatalogCollectionWaitsForUnknownQuarantineReferences) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now())
                    .ok());
    const auto unknown = volume_->artifacts_directory().parent_path() / "quarantine/unknown";
    ASSERT_TRUE(fs::create_directory(unknown));
    EXPECT_EQ(
        catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s)
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    ASSERT_TRUE(fs::remove(unknown));
    EXPECT_TRUE(
        catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s).value());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
}

class CatalogCollectionResurrectionTest : public PipelineControllerTest,
                                          public testing::WithParamInterface<bool> {};

TEST_P(CatalogCollectionResurrectionTest, ReappearedFinalOrStagingCannotEraseTheTombstone) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now())
                    .ok());
    const auto path = volume_->artifacts_directory() / (GetParam() ? "7" : "7.tmp");
    ASSERT_TRUE(fs::create_directory(path));
    EXPECT_EQ(
        catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s)
            .status()
            .code(),
        StatusCode::DataLoss);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
    EXPECT_EQ(capacity_->snapshot().protection, StatusCode::DataLoss);
}

INSTANTIATE_TEST_SUITE_P(UnknownNames, CatalogCollectionResurrectionTest, testing::Bool());

TEST_F(PipelineControllerTest, CatalogCollectionCannotTreatUnexpectedLedgerAbsenceAsPriorGC) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now())
                    .ok());
    ASSERT_TRUE(fs::remove(root_ / "ledger/07/7.json"));
    const auto attempted =
        catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s);
    EXPECT_EQ(attempted.status().code(), StatusCode::DataLoss);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
    EXPECT_EQ(capacity_->snapshot().protection, StatusCode::DataLoss);
    EXPECT_EQ(catalog_->snapshot().protection, StatusCode::DataLoss);
}

TEST_F(PipelineControllerTest, CatalogCollectionDoesNotCreditCapacityBeforeEitherDirectorySync) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now())
                    .ok());
    *delete_failed_step_ = static_cast<int>(artifact::DeleteStep::SyncParent);
    EXPECT_FALSE(
        catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s).ok());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    *delete_failed_step_ = -1;
    *ledger_fault_ = true;
    EXPECT_FALSE(
        catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s).ok());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
    EXPECT_EQ(ledger_->get(7).status().code(), StatusCode::Unavailable);
    EXPECT_EQ(catalog_->snapshot().pending, 0);
    *ledger_fault_ = false;
    // A prior admitted collection keeps its ownership despite a wall-clock jump.
    EXPECT_TRUE(
        catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{0s}, 1s).value());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
}

TEST_F(PipelineControllerTest, CatalogCollectionRetainsActualFlightUntilParentSyncReturns) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now())
                    .ok());
    delete_control_->block = true;
    auto actual = std::async(std::launch::async, [&] {
        return catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s);
    });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(delete_control_->mutex);
        entered =
            delete_control_->changed.wait_for(lock, 2s, [&] { return delete_control_->entered; });
    }
    if (entered) {
        EXPECT_EQ(actual.wait_for(1ms), std::future_status::timeout);
        EXPECT_EQ(catalog_->snapshot().collect_inflight, 1);
        EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
        EXPECT_FALSE(
            catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s)
                .value());
        EXPECT_TRUE(artifacts_->query_artifacts({}).ok());
        EXPECT_EQ(
            catalog_
                ->delete_artifact(
                    {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{2s},
                    std::chrono::steady_clock::now())
                .status()
                .code(),
            StatusCode::Conflict);
    }
    {
        std::lock_guard<std::mutex> lock(delete_control_->mutex);
        delete_control_->released = true;
    }
    delete_control_->changed.notify_all();
    const auto completed = actual.get();
    EXPECT_TRUE(entered);
    ASSERT_TRUE(completed.ok());
    EXPECT_TRUE(completed.value());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
    EXPECT_EQ(catalog_->snapshot().collect_inflight, 0);
}

TEST_F(PipelineControllerTest, CatalogCollectionRechecksNamesAfterActualParentSyncReturns) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention, rodaq::WallTime{1s},
                        std::chrono::steady_clock::now())
                    .ok());
    delete_control_->block = true;
    auto actual = std::async(std::launch::async, [&] {
        return catalog_->collect_deleted(options_.identity.task_id, 0, rodaq::WallTime{2s}, 1s);
    });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(delete_control_->mutex);
        entered =
            delete_control_->changed.wait_for(lock, 2s, [&] { return delete_control_->entered; });
    }
    const auto reappeared = volume_->artifacts_directory() / "7.tmp";
    const bool created = entered && fs::create_directory(reappeared);
    {
        std::lock_guard<std::mutex> lock(delete_control_->mutex);
        delete_control_->released = true;
    }
    delete_control_->changed.notify_all();
    const auto completed = actual.get();
    ASSERT_TRUE(entered);
    ASSERT_TRUE(created);
    EXPECT_EQ(completed.status().code(), StatusCode::DataLoss);
    EXPECT_TRUE(fs::exists(reappeared));
    EXPECT_EQ(ledger_->get(7).value().state, artifact::LedgerState::Deleted);
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
}

TEST_F(PipelineControllerTest, CatalogCollectionAuditArithmeticHandlesOppositeWallTimeExtremes) {
    record_committed();
    ASSERT_TRUE(catalog_
                    ->delete_artifact(
                        {7}, 1, artifact::DeleteReason::Retention,
                        rodaq::WallTime{std::chrono::nanoseconds::min()},
                        std::chrono::steady_clock::now())
                    .ok());
    const auto collected = catalog_->collect_deleted(
        options_.identity.task_id, 0, rodaq::WallTime{std::chrono::nanoseconds::max()},
        std::chrono::nanoseconds::max());
    ASSERT_TRUE(collected.ok());
    EXPECT_TRUE(collected.value());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
}

TEST_F(PipelineControllerTest, ShutdownRetainsActualCollectionUntilLedgerUnlinkReturns) {
    open_composition(&receive_clock, 5s, 1, 1, 1ns);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recording_mode = rodaq::RecordingMode::Rolling;
    config.retention_policy = {rodaq::RetentionMode::Rolling, {}, 1};
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    ASSERT_TRUE(wait_composed_records(id));
    const auto stop = service.stop(id, {"stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    *ledger_blocked_step_ = rodaq::persistence::FileStep::RemoveAuthority;
    ledger_control_->block = true;
    auto maintenance = std::async(std::launch::async, [&] { return composition_->observe(); });
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(ledger_control_->mutex);
        entered =
            ledger_control_->changed.wait_for(lock, 2s, [&] { return ledger_control_->entered; });
    }
    const auto observed = composition_->shutdown(std::chrono::steady_clock::now() + 10ms);
    if (entered) {
        EXPECT_EQ(capacity_->snapshot().artifact_slots, 1);
        EXPECT_EQ(maintenance.wait_for(1ms), std::future_status::timeout);
    }
    {
        std::lock_guard<std::mutex> lock(ledger_control_->mutex);
        ledger_control_->released = true;
    }
    ledger_control_->changed.notify_all();
    const auto returned = maintenance.get();
    ASSERT_TRUE(entered);
    EXPECT_FALSE(observed.ok());
    EXPECT_TRUE(returned.ok());
    EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
    EXPECT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(service.get_task(id).value().control_generation, 3);
}

TEST_F(PipelineControllerTest, AutomaticCollectionReusesCapacityWithoutReusingSegmentIdentity) {
    open_composition(&receive_clock, 5s, 1, 1, 1ns);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    config.recording_mode = rodaq::RecordingMode::Rolling;
    config.retention_policy = {rodaq::RetentionMode::Rolling, {}, 1};
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    std::uint64_t previous_segment = 0;
    for (std::uint64_t i = 0; i < 24; ++i) {
        const auto start = service.start(id, {"start-" + std::to_string(i)}, {true, 1 + i * 2});
        ASSERT_TRUE(start.ok());
        ASSERT_TRUE(wait_composed(id, start.value()));
        ASSERT_TRUE(wait_composed_records(id));
        const auto stop = service.stop(id, {"stop-" + std::to_string(i)}, {true, 2 + i * 2});
        ASSERT_TRUE(stop.ok());
        ASSERT_TRUE(wait_composed(id, stop.value()));
        const auto artifacts = composition_->artifacts().query_artifacts({});
        ASSERT_TRUE(artifacts.ok());
        ASSERT_EQ(artifacts.value().items.size(), 1);
        const auto segment = artifacts.value().items[0].segment_id;
        EXPECT_GT(segment, previous_segment);
        previous_segment = segment;
        ASSERT_TRUE(composition_->observe().ok());
        EXPECT_EQ(ledger_->get(segment).status().code(), StatusCode::NotFound);
        EXPECT_EQ(capacity_->snapshot().artifact_slots, 0);
        EXPECT_EQ(capacity_->snapshot().actual_bytes, 0);
        // Exercise three times the deployment's eight artifact/ledger slots,
        // while respecting the existing Recovery publication rate contract.
        std::this_thread::sleep_for(250ms);
    }
}

class RecordingTimelineTest : public PipelineControllerTest,
                              public testing::WithParamInterface<bool> {};

TEST_P(RecordingTimelineTest, FormalResumeKeepsTaskRecordingTimeAcrossPausedHostTimeAndDrain) {
    retention_monotonic = 200;
    open_composition(&retention_clock, 5s, 1, GetParam() ? 2 : 1);
    ASSERT_TRUE(composition_);
    auto& service = composition_->tasks();
    auto config = formal_config();
    if (GetParam()) {
        config.pipelines.push_back(config.pipelines.front());
    }
    const auto created = service.create_task(config, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    const auto wait_receive = [&](std::int64_t realtime, const auto& before) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            if (!composition_->observe().ok()) {
                return false;
            }
            const auto runtime = service.get_runtime_snapshot(id);
            bool ready =
                runtime.ok() && runtime.value().pipelines.size() == before.pipelines.size();
            for (std::size_t i = 0; ready && i < before.pipelines.size(); ++i) {
                const auto& pipeline = runtime.value().pipelines[i];
                ready = pipeline.facts && before.pipelines[i].facts &&
                        pipeline.facts->accepted_records >=
                            before.pipelines[i].facts->accepted_records + 4;
                for (const auto& stream : pipeline.streams) {
                    ready = ready && stream.facts &&
                            stream.facts->last_receive_at ==
                                rodaq::WallTime{std::chrono::nanoseconds{realtime}};
                }
            }
            if (ready) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    ASSERT_TRUE(composition_->observe().ok());
    auto before = service.get_runtime_snapshot(id).value();
    retention_monotonic = 500;
    ASSERT_TRUE(wait_receive(1400, before));
    ledger_control_->block = true;
    const auto pause = service.pause(id, {"pause"}, {true, 2});
    bool entered = false;
    if (pause.ok()) {
        std::unique_lock<std::mutex> lock(ledger_control_->mutex);
        entered =
            ledger_control_->changed.wait_for(lock, 2s, [&] { return ledger_control_->entered; });
    }
    retention_monotonic = 8000;
    {
        std::lock_guard<std::mutex> lock(ledger_control_->mutex);
        ledger_control_->released = true;
    }
    ledger_control_->changed.notify_all();
    ASSERT_TRUE(pause.ok());
    ASSERT_TRUE(entered);
    ASSERT_TRUE(wait_composed(id, pause.value()));
    const auto first = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(first.ok());
    ASSERT_EQ(first.value().items.size(), GetParam() ? 2 : 1);
    for (const auto& artifact : first.value().items) {
        EXPECT_EQ(artifact.recording_range.end_ns, 300);
    }
    const auto saved = rodaq::persistence::read_file(
        root_ / "service/recovery", rodaq::to_string(id) + ".json", 262144);
    ASSERT_TRUE(saved.ok());
    EXPECT_EQ(
        rodaq::task::decode_recovery_record(saved.value(), {}).value().recording_time_ns, 300);
    retention_monotonic = 10000;
    ASSERT_TRUE(composition_->observe().ok());
    const auto resume = service.resume(id, {"resume"}, {true, 3});
    ASSERT_TRUE(resume.ok());
    ASSERT_TRUE(wait_composed(id, resume.value()));
    ASSERT_TRUE(composition_->observe().ok());
    before = service.get_runtime_snapshot(id).value();
    retention_monotonic = 10300;
    ASSERT_TRUE(wait_receive(11200, before));
    const auto stop = service.stop(id, {"stop"}, {true, 4});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    const auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), GetParam() ? 4 : 2);
    for (const auto& artifact : artifacts.value().items) {
        const bool previous = std::any_of(
            first.value().items.begin(), first.value().items.end(),
            [&](const auto& old) { return old.artifact_id == artifact.artifact_id; });
        if (!previous) {
            EXPECT_GE(artifact.recording_range.begin_ns, 300);
            EXPECT_EQ(artifact.recording_range.end_ns, 600);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(SharedTaskTime, RecordingTimelineTest, testing::Bool());

TEST_F(PipelineControllerTest, SilentOpenTimeIsPersistedWithoutCreatingCommittedProgress) {
    retention_monotonic = 200;
    open_composition(&retention_clock);
    ASSERT_TRUE(composition_);
    auto config = formal_config();
    config.pipelines[0].acquisition.json = R"({"streams":["first","second"],"mode":"silent"})";
    auto& service = composition_->tasks();
    const auto id = service.create_task(config, {"create"}).value();
    const auto start = service.start(id, {"start"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_composed(id, start.value()));
    retention_monotonic = 500;
    const auto pause = service.pause(id, {"pause"}, {true, 2});
    ASSERT_TRUE(pause.ok());
    ASSERT_TRUE(wait_composed(id, pause.value()));
    auto bytes = rodaq::persistence::read_file(
        root_ / "service/recovery", rodaq::to_string(id) + ".json", 262144);
    ASSERT_TRUE(bytes.ok());
    auto recovered = rodaq::task::decode_recovery_record(bytes.value(), {});
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().recording_time_ns, 300);
    retention_monotonic = 10000;
    const auto resume = service.resume(id, {"resume"}, {true, 3});
    ASSERT_TRUE(resume.ok());
    ASSERT_TRUE(wait_composed(id, resume.value()));
    retention_monotonic = 10300;
    const auto stop = service.stop(id, {"stop"}, {true, 4});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_composed(id, stop.value()));
    bytes = rodaq::persistence::read_file(
        root_ / "service/recovery", rodaq::to_string(id) + ".json", 262144);
    ASSERT_TRUE(bytes.ok());
    recovered = rodaq::task::decode_recovery_record(bytes.value(), {});
    ASSERT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.value().recording_time_ns, 600);
    EXPECT_TRUE(composition_->artifacts().query_artifacts({}).value().items.empty());
    EXPECT_TRUE(fs::is_empty(volume_->artifacts_directory()));
}

TEST_F(PipelineControllerTest, SampledCutCannotRegressBelowAnActuallyAcceptedRecord) {
    retention_monotonic = 200;
    options_.receive_clock = &retention_clock;
    create();
    ASSERT_TRUE(controller_);
    const auto prepared = controller_->prepare(soon());
    ASSERT_TRUE(prepared.ok());
    const auto authorization = authorize(prepared.value());
    retention_monotonic = 1200;
    ASSERT_EQ(controller_->open_admission(authorization), pipeline::GateOpenResult::Opened);
    ASSERT_TRUE(wait_written(4));
    // The control caller sampled 500 before a later candidate committed at
    // 1200. Its fence must preserve the record's already accepted Task time.
    ASSERT_TRUE(controller_->establish_control_fence(500).ok());
    ASSERT_TRUE(controller_->close_admission(8).ok());
    auto sealed = controller_->drain(soon());
    ASSERT_TRUE(sealed.ok());
    ASSERT_TRUE(sealed.value());
    commit(std::move(*sealed.value()));
    ASSERT_TRUE(controller_->quiesce(soon()).ok());
    const auto boundary = controller_->recording_time_boundary();
    ASSERT_TRUE(boundary.ok());
    ASSERT_TRUE(boundary.value());
    EXPECT_EQ(*boundary.value(), 1000);
}

}  // namespace
