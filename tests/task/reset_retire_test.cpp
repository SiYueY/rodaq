#include "daq/composition.hpp"
#include "daq/startup.hpp"
#include "artifact/artifact_catalog.hpp"
#include "persistence/json.hpp"
#include "task/identity.hpp"
#include "task/retirement_store.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rodaq::ArtifactId;
using rodaq::ArtifactReadHandle;
using rodaq::ControlPrecondition;
using rodaq::DesiredState;
using rodaq::ExecutionCondition;
using rodaq::OperationId;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::RequestIdentity;
using rodaq::StatusCode;
using rodaq::TaskConfig;
using rodaq::TaskId;
using rodaq::daq::Composition;
using rodaq::daq::CompositionOptions;
using rodaq::daq::DeploymentOpenMode;
using rodaq::daq::StartupOptions;

class ResetRetireTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 48> pattern{};
        const std::string prefix = "/tmp/rodaq-reset-retire-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        fs::create_directory(root_ / "packages");
        fs::create_directory(root_ / "state");
        fs::create_directory(root_ / "volume");
        install_package(RODAQ_RESET_STORAGE_PACKAGE, "rodaq.fake.storage");
        install_package(RODAQ_RESET_ACQUISITION_PACKAGE, "rodaq.fake.acquisition");
        set_readonly_tree(root_ / "packages");
    }

    void TearDown() override {
        if (composition_) {
            (void)composition_->shutdown(std::chrono::steady_clock::now() + 5s);
            composition_.reset();
        }
        std::error_code ignored;
        restore_writable_tree(root_ / "packages");
        fs::remove_all(root_, ignored);
    }

    static void set_readonly_tree(const fs::path& root) {
        std::vector<fs::path> dirs;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_directory()) {
                dirs.push_back(entry.path());
            } else {
                fs::permissions(entry.path(), fs::perms::owner_read);
            }
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
            fs::permissions(*it, fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec);
    }

    static void restore_writable_tree(const fs::path& root) noexcept {
        std::error_code ignored;
        if (!fs::exists(root, ignored)) {
            return;
        }
        fs::permissions(root, fs::perms::owner_all, ignored);
        for (const auto& entry : fs::recursive_directory_iterator(root, ignored)) {
            fs::permissions(entry.path(), fs::perms::owner_all, ignored);
        }
    }

    void install_package(const char* source_root, const char* plugin_id) {
        const fs::path src = fs::path(source_root) / plugin_id;
        const fs::path dst = root_ / "packages" / plugin_id;
        fs::copy(src, dst, fs::copy_options::recursive);
    }

    StartupOptions make_startup(DeploymentOpenMode mode) {
        StartupOptions options;
        options.state_root = root_ / "state";
        options.volume_root = root_ / "volume";
        options.mode = mode;
        options.plugins.search_paths = {root_ / "packages"};
        options.plugins.max_loaded_plugins = 4;
        options.plugins.max_instances = 8;
        options.plugins.max_config_size = 4096;
        options.plugins.shutdown_timeout = 2s;
        options.capacity = {1000000, 1000000, 100, 2000000000, 16, 16, 32};
        options.ledger_records = 16;

        auto& s = options.settings;
        s.deployment_generation = 1;
        s.qualified_runtime_profile_id = "test-reset-retire";
        s.output_root = "local";
        s.max_tasks = 2;
        s.create_recovery_slots = 4;
        s.native_timeout = 2s;
        s.artifact_audit_window = 1ns;
        s.tasks.config.max_pipelines = 1;

        rodaq::daq::ResourceProfile profile;
        profile.name = "basic";
        profile.instance.records = 32;
        profile.instance.logical_bytes = 256;
        profile.instance.payloads = {16384, 256, 8, 32};
        profile.instance.queue_capacity = 16;
        profile.instance.callback_concurrency = 2;
        profile.storage = {{sizeof(rodaq::storage::PrepareConfig)},
                           8,
                           24,
                           3,
                           8,
                           100,
                           800,
                           100000,
                           1,
                           32,
                           1,
                           100,
                           1000};
        profile.manifest.max_bytes = 65536;
        profile.max_prepared_descriptors = 16;
        profile.sequence_reservation = 1000;
        profile.descriptor_reservation = 1000;
        s.profiles.push_back(profile);

        s.storage.push_back({"rodaq.fake.storage", "1.0.0", "fake"});
        s.allowed_plugins = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        return options;
    }

    TaskConfig make_task_config(std::string name = "reset-retire-task") {
        TaskConfig config;
        config.name = std::move(name);
        config.recording_mode = rodaq::RecordingMode::Continuous;
        config.start_policy = rodaq::StartPolicyMode::AllRequired;
        config.recovery_policy = rodaq::RecoveryPolicyMode::Manual;
        config.segment_policy.max_duration = 60s;
        config.retention_policy.mode = rodaq::RetentionMode::KeepAll;

        rodaq::PipelineConfig pipe;
        pipe.required = true;
        pipe.resource_profile = "basic";
        pipe.output_root = "local";
        pipe.acquisition = {
            "rodaq.fake.acquisition", "1.0.0", R"({"streams":["sample"],"period_us":1000})"};
        pipe.storage = {"rodaq.fake.storage", "1.0.0", "{}"};
        pipe.streams.push_back({0, "sample", "sample", rodaq::ReadinessPolicy::EntryReady});
        config.pipelines.push_back(std::move(pipe));
        return config;
    }

    bool wait_operation(
        TaskId task, OperationId op, OperationState expected = OperationState::Succeeded,
        bool drive_observe = false) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto result = composition_->tasks().get_operation(task, op);
            if (result.ok() && rodaq::task::terminal(result.value().state)) {
                return result.value().state == expected;
            }
            if (drive_observe) {
                (void)composition_->observe();
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    bool wait_records(TaskId task, std::uint64_t min_records = 1) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            (void)composition_->observe();
            auto rt = composition_->tasks().get_runtime_snapshot(task);
            if (rt.ok() && !rt.value().pipelines.empty() && rt.value().pipelines[0].facts &&
                rt.value().pipelines[0].facts->accepted_records >= min_records) {
                return true;
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    void record_one_segment(TaskId task, std::uint64_t& gen, const std::string& prefix) {
        auto start = composition_->tasks().start(task, {prefix + "-start"}, {true, gen});
        ASSERT_TRUE(start.ok()) << static_cast<int>(start.status().code());
        if (!wait_operation(task, start.value())) {
            auto snap = composition_->tasks().get_operation(task, start.value());
            auto rt = composition_->tasks().get_runtime_snapshot(task);
            int pstat = -1;
            if (rt.ok() && !rt.value().pipelines.empty() && rt.value().pipelines[0].facts &&
                rt.value().pipelines[0].facts->prepare_status) {
                pstat = static_cast<int>(*rt.value().pipelines[0].facts->prepare_status);
            }
            ADD_FAILURE() << "start failed: state=" << static_cast<int>(snap.value().state)
                          << " status=" << static_cast<int>(snap.value().status_code)
                          << " pstat=" << pstat << " diag=" << snap.value().diagnostic;
            return;
        }
        ++gen;
        ASSERT_TRUE(wait_records(task, 1));
        auto stop = composition_->tasks().stop(task, {prefix + "-stop"}, {true, gen});
        ASSERT_TRUE(stop.ok());
        ASSERT_TRUE(wait_operation(task, stop.value()));
        ++gen;
    }

    fs::path root_;
    std::unique_ptr<Composition> composition_;
};

TEST_F(ResetRetireTest, ResetDeletesArtifactsPreservesStreamHighWaterAndAllowsNextRun) {
    auto opened = Composition::open(make_startup(DeploymentOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    auto task_res = composition_->tasks().create_task(make_task_config(), {"create-1"});
    ASSERT_TRUE(task_res.ok());
    const TaskId task = task_res.value();
    std::uint64_t gen = 1;

    record_one_segment(task, gen, "run1");

    auto before_artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(before_artifacts.ok());
    ASSERT_EQ(before_artifacts.value().items.size(), 1u);
    const auto first_artifact_id = before_artifacts.value().items[0].artifact_id;
    const auto first_info = composition_->artifacts().get_artifact(first_artifact_id);
    ASSERT_TRUE(first_info.ok());
    ASSERT_FALSE(first_info.value().coverage.empty());
    const auto first_last_seq = first_info.value().coverage[0].last_sequence;
    const auto first_seg_id = first_info.value().summary.segment_id;

    // Stale precondition is rejected atomically without side effects.
    EXPECT_EQ(
        composition_->tasks().reset(task, {"reset-stale"}, {true, gen - 1}).status().code(),
        StatusCode::Conflict);

    // Formal Reset deletes Task artifacts and advances reset_generation.
    auto reset_op = composition_->tasks().reset(task, {"reset-1"}, {true, gen});
    ASSERT_TRUE(reset_op.ok());
    ASSERT_TRUE(wait_operation(task, reset_op.value()));
    ++gen;

    // Idempotent retry returns the same operation_id even after generation advanced.
    auto reset_retry = composition_->tasks().reset(task, {"reset-1"}, {true, gen - 1});
    ASSERT_TRUE(reset_retry.ok());
    EXPECT_EQ(reset_retry.value(), reset_op.value());

    // Conflicting digest with same key returns Conflict.
    EXPECT_EQ(
        composition_->tasks().reset(task, {"reset-1"}, {true, gen}).status().code(),
        StatusCode::Conflict);

    auto snap = composition_->tasks().get_task(task);
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(snap.value().reset_generation, 1u);
    EXPECT_EQ(snap.value().execution_condition, ExecutionCondition::Idle);

    auto after_reset_artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(after_reset_artifacts.ok());
    EXPECT_TRUE(after_reset_artifacts.value().items.empty());

    // Start/Stop again: Stream AcceptanceSequence high-water and SegmentId MUST NOT regress.
    record_one_segment(task, gen, "run2");
    auto second_artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(second_artifacts.ok());
    ASSERT_EQ(second_artifacts.value().items.size(), 1u);
    const auto second_info =
        composition_->artifacts().get_artifact(second_artifacts.value().items[0].artifact_id);
    ASSERT_TRUE(second_info.ok());
    EXPECT_GT(second_info.value().summary.segment_id, first_seg_id);
    ASSERT_FALSE(second_info.value().coverage.empty());
    EXPECT_GT(second_info.value().coverage[0].first_sequence, first_last_seq);
}

TEST_F(ResetRetireTest, ResetBlockedByActiveReadLeaseThenSettlesAfterRelease) {
    auto startup = make_startup(DeploymentOpenMode::Initialize);
    startup.settings.native_timeout = 100ms;
    auto opened = Composition::open(std::move(startup));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    auto task = composition_->tasks().create_task(make_task_config(), {"create-lease"}).value();
    std::uint64_t gen = 1;
    record_one_segment(task, gen, "run1");

    auto list = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().items.size(), 1u);
    const auto aid = list.value().items[0].artifact_id;
    auto info = composition_->artifacts().get_artifact(aid);
    ASSERT_TRUE(info.ok());
    ASSERT_FALSE(info.value().files.empty());

    // Hold an active ReadLease while initiating Reset.
    auto opened_handle = composition_->artifacts().acquire_read(aid, info.value().files[0].file_id);
    ASSERT_TRUE(opened_handle.ok());
    std::optional<ArtifactReadHandle> handle(std::move(opened_handle.value()));

    auto reset_op = composition_->tasks().reset(task, {"reset-lease"}, {true, gen});
    ASSERT_TRUE(reset_op.ok());

    // Wait past native_timeout so ResetJob has attempted deletion and yielded.
    std::this_thread::sleep_for(250ms);
    (void)composition_->observe();

    // Reset MUST NOT declare Succeeded while ReadLease is held!
    auto op_snap = composition_->tasks().get_operation(task, reset_op.value());
    ASSERT_TRUE(op_snap.ok());
    EXPECT_FALSE(rodaq::task::terminal(op_snap.value().state));

    // While Reset is in progress, Start, Configure, and Retire are blocked!
    EXPECT_EQ(
        composition_->tasks().start(task, {"blocked-start"}, {}).status().code(),
        StatusCode::InvalidState);
    EXPECT_EQ(
        composition_->tasks().retire(task, {"blocked-retire"}, {}).status().code(),
        StatusCode::Conflict);

    // Release the ReadLease -> subsequent observe() retries and settles Reset!
    handle.reset();
    ASSERT_TRUE(wait_operation(task, reset_op.value(), OperationState::Succeeded, true));

    EXPECT_TRUE(composition_->artifacts().query_artifacts({}).value().items.empty());
    EXPECT_EQ(
        composition_->tasks().get_task(task).value().execution_condition, ExecutionCondition::Idle);
}

TEST_F(ResetRetireTest, RetireEnforcesArtifactEligibilityReleasesSlotAndRecoversAcrossRestart) {
    auto startup = make_startup(DeploymentOpenMode::Initialize);
    startup.settings.max_tasks = 1;
    startup.settings.artifact_audit_window = 1ns;
    auto opened = Composition::open(std::move(startup));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId task = composition_->tasks().create_task(make_task_config("t1"), {"c1"}).value();
    // Active max_tasks=1 is full: creating a second Task fails with ResourceExhausted.
    EXPECT_EQ(
        composition_->tasks().create_task(make_task_config("t2"), {"c2"}).status().code(),
        StatusCode::ResourceExhausted);

    std::uint64_t gen = 1;
    record_one_segment(task, gen, "run1");

    // Retire MUST be rejected while a Committed artifact exists!
    EXPECT_EQ(
        composition_->tasks().retire(task, {"retire-blocked"}, {true, gen}).status().code(),
        StatusCode::Conflict);

    // Reset clears the committed artifact (leaving Deleted tombstone only).
    auto reset_op = composition_->tasks().reset(task, {"reset-before-retire"}, {true, gen});
    ASSERT_TRUE(reset_op.ok());
    ASSERT_TRUE(wait_operation(task, reset_op.value()));
    ++gen;

    // Now TaskRetireEligible == true (Deleted tombstone only) -> Retire succeeds!
    auto retire_op = composition_->tasks().retire(task, {"retire-1"}, {true, gen});
    ASSERT_TRUE(retire_op.ok());
    ASSERT_TRUE(wait_operation(task, retire_op.value(), OperationState::Succeeded));

    // Retired Task is no longer in active Task catalog.
    EXPECT_EQ(composition_->tasks().get_task(task).status().code(), StatusCode::NotFound);
    EXPECT_TRUE(composition_->tasks().list_tasks({}).value().items.empty());

    // get_operation() recovers Succeeded directly from retirement tombstone!
    auto op_after = composition_->tasks().get_operation(task, retire_op.value());
    ASSERT_TRUE(op_after.ok());
    EXPECT_EQ(op_after.value().kind, OperationKind::Retire);
    EXPECT_EQ(op_after.value().state, OperationState::Succeeded);

    // Idempotent Retire retry returns original operation_id; conflicting digest returns Conflict.
    auto retire_retry = composition_->tasks().retire(task, {"retire-1"}, {true, gen});
    ASSERT_TRUE(retire_retry.ok());
    EXPECT_EQ(retire_retry.value(), retire_op.value());
    EXPECT_EQ(
        composition_->tasks().retire(task, {"retire-1"}, {true, gen + 1}).status().code(),
        StatusCode::Conflict);

    // Different key on retired Task cannot re-retire or reactivate it.
    EXPECT_EQ(
        composition_->tasks().retire(task, {"retire-other"}, {}).status().code(),
        StatusCode::NotFound);
    EXPECT_EQ(
        composition_->tasks().start(task, {"start-retired"}, {}).status().code(),
        StatusCode::NotFound);

    // Active max_tasks=1 slot was released: creating a new Task succeeds!
    auto task2 = composition_->tasks().create_task(make_task_config("t2"), {"c2"});
    ASSERT_TRUE(task2.ok());
    EXPECT_FALSE(task2.value() == task);

    // Restart daemon: retired Task MUST NOT resurrect, and Retire op remains queryable
    // while tombstone is still retained (until Artifact Deleted tombstone is also GC'd).
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 5s).ok());
    composition_.reset();

    auto reopened_opts = make_startup(DeploymentOpenMode::Existing);
    reopened_opts.settings.max_tasks = 1;
    reopened_opts.settings.artifact_audit_window = 24h;  // Prevent GC on reopen.
    auto reopened = Composition::open(std::move(reopened_opts));
    ASSERT_TRUE(reopened.ok()) << "reopen code=" << static_cast<int>(reopened.status().code())
                               << " msg=" << reopened.status().message();
    composition_ = std::move(reopened.value());

    EXPECT_EQ(composition_->tasks().get_task(task).status().code(), StatusCode::NotFound);
    auto op_reopened = composition_->tasks().get_operation(task, retire_op.value());
    ASSERT_TRUE(op_reopened.ok());
    EXPECT_EQ(op_reopened.value().state, OperationState::Succeeded);
    EXPECT_TRUE(composition_->tasks().get_task(task2.value()).ok());
}

TEST_F(ResetRetireTest, RetireCrashBeforeAndAfterTombstoneMaintainExactLinearizationPoint) {
    // Use TaskService directly to inject deterministic RetireStep crash points!
    auto create_lane = rodaq::ExecutionLane::create(1, 8).value();
    auto control_lane = rodaq::ExecutionLane::create(1, 8).value();
    auto prep_lane = rodaq::ExecutionLane::create(2, 8).value();
    auto cleanup_lane = rodaq::ExecutionLane::create(2, 8).value();

    auto open_plugins = [&]() {
        rodaq::plugin::PluginManagerOptions po;
        po.search_paths = {root_ / "packages"};
        po.max_loaded_plugins = 4;
        po.max_instances = 8;
        po.max_config_size = 4096;
        po.shutdown_timeout = 2s;
        return std::make_shared<rodaq::plugin::PluginManager>(
            rodaq::plugin::PluginManager::create(std::move(po)).value());
    };
    auto plugins = open_plugins();
    struct LaneGuard {
        std::shared_ptr<rodaq::ExecutionLane> create_l;
        std::shared_ptr<rodaq::ExecutionLane> control_l;
        std::shared_ptr<rodaq::ExecutionLane> prep_l;
        std::shared_ptr<rodaq::ExecutionLane> cleanup_l;
        std::shared_ptr<rodaq::plugin::PluginManager> mgr;
        ~LaneGuard() {
            const auto dl = std::chrono::steady_clock::now() + 2s;
            (void)create_l->shutdown(dl);
            (void)control_l->shutdown(dl);
            (void)prep_l->shutdown(dl);
            (void)cleanup_l->shutdown(dl);
            if (auto pdl = rodaq::plugin::Deadline::after(2s); pdl.ok()) {
                (void)mgr->shutdown(pdl.value());
            }
        }
    } guard{create_lane, control_lane, prep_lane, cleanup_lane, plugins};
    const fs::path svc_root = root_ / "svc-crash";
    fs::create_directory(svc_root);

    std::atomic<bool> block_before_tombstone{true};
    std::atomic<bool> block_after_tombstone{false};
    std::atomic<bool> block_cleanup{false};

    auto make_service = [&]() {
        rodaq::task::ServiceOptions opts;
        opts.root = svc_root;
        opts.plugin_manager = plugins;
        opts.create_lane = create_lane;
        opts.control_lane = control_lane;
        opts.max_tasks = 2;
        opts.resource_profiles = {{"basic", 1000000}};
        opts.output_roots = {"local"};
        opts.allowed_plugins = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        auto exec = std::make_shared<rodaq::task::RunExecutionOptions>();
        exec->preparation_lane = prep_lane;
        exec->cleanup_lane = cleanup_lane;
        exec->resolve = [](const auto&, const auto&, auto&) {
            return rodaq::Result<rodaq::task::RunInputs>(
                rodaq::Status(StatusCode::Unavailable, {}));
        };
        exec->commit = [](const auto&) { return rodaq::Result<void>{}; };
        exec->reserve_segment = []() { return rodaq::Result<std::uint64_t>(1); };
        exec->artifacts.retire_eligible = [](TaskId) { return rodaq::Result<bool>(true); };
        exec->artifacts.has_task_references = [](TaskId) { return rodaq::Result<bool>(false); };
        opts.execution = std::move(exec);
        opts.retire_faults = [&](rodaq::task::RetireStep step) {
            if (step == rodaq::task::RetireStep::BeforeTombstone && block_before_tombstone) {
                return EIO;
            }
            if (step == rodaq::task::RetireStep::AfterTombstone && block_after_tombstone) {
                return EIO;
            }
            if (step == rodaq::task::RetireStep::NamespaceCleanup && block_cleanup) {
                return EIO;
            }
            return 0;
        };
        return rodaq::TaskService::open(std::move(opts));
    };

    // Phase 1: Crash BEFORE tombstone publish.
    TaskId task;
    OperationId retire_op;
    {
        auto svc = make_service();
        ASSERT_TRUE(svc.ok());
        task = svc.value()->create_task(make_task_config("crash-retire"), {"c-crash"}).value();
        auto op = svc.value()->retire(task, {"r-crash"}, {true, 1});
        ASSERT_TRUE(op.ok());
        retire_op = op.value();
        // Wait for ControlJob to dispatch RetireJob, then wait for RetireJob to settle.
        ASSERT_TRUE(create_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        ASSERT_TRUE(control_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        ASSERT_TRUE(cleanup_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        ASSERT_TRUE(control_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        // Tombstone MUST NOT exist and Retire MUST NOT be Succeeded!
        EXPECT_FALSE(fs::exists(svc_root / "retired" / (rodaq::to_string(task) + ".json")));
        auto op_snap = svc.value()->get_operation(task, retire_op);
        ASSERT_TRUE(op_snap.ok());
        EXPECT_NE(op_snap.value().state, OperationState::Succeeded);
    }

    // Phase 2: Reopen -> Retire is still active (never fabricated Succeeded).
    // Retry Retire -> now allow tombstone publish, crash IMMEDIATELY AFTER tombstone!
    block_before_tombstone = false;
    block_after_tombstone = true;
    {
        auto svc = make_service();
        ASSERT_TRUE(svc.ok()) << "phase2 open code=" << static_cast<int>(svc.status().code())
                              << " msg=" << svc.status().message();
        ASSERT_TRUE(svc.value()->get_task(task).ok());
        auto retry = svc.value()->retire(task, {"r-crash"}, {true, 1});
        ASSERT_TRUE(retry.ok());
        EXPECT_EQ(retry.value(), retire_op);
        ASSERT_TRUE(control_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        ASSERT_TRUE(cleanup_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        ASSERT_TRUE(create_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        ASSERT_TRUE(control_lane->wait_idle(std::chrono::steady_clock::now() + 2s).ok());
        // Tombstone IS durable now, active files still remain on disk (simulated crash after
        // tombstone)!
        EXPECT_TRUE(fs::exists(svc_root / "retired" / (rodaq::to_string(task) + ".json")));
        EXPECT_TRUE(fs::exists(svc_root / "control" / (rodaq::to_string(task) + ".json")));
        // Task is already gone from active catalog and Retire is Succeeded!
        EXPECT_EQ(svc.value()->get_task(task).status().code(), StatusCode::NotFound);
        auto op_snap = svc.value()->get_operation(task, retire_op);
        ASSERT_TRUE(op_snap.ok());
        EXPECT_EQ(op_snap.value().state, OperationState::Succeeded);
    }

    // Phase 3: Reopen with old control/config files STILL present on disk!
    // Task MUST NOT resurrect, and Retire MUST remain Succeeded from tombstone!
    block_after_tombstone = false;
    {
        auto svc = make_service();
        ASSERT_TRUE(svc.ok());
        EXPECT_EQ(svc.value()->get_task(task).status().code(), StatusCode::NotFound);
        EXPECT_TRUE(svc.value()->list_tasks({}).value().items.empty());
        auto op_snap = svc.value()->get_operation(task, retire_op);
        ASSERT_TRUE(op_snap.ok());
        EXPECT_EQ(op_snap.value().state, OperationState::Succeeded);
    }
}

}  // namespace
