#include "daq/composition.hpp"
#include "daq/startup.hpp"
#include "artifact/ledger_store.hpp"
#include "persistence/atomic_file.hpp"
#include "task/control_record.hpp"
#include "task/identity.hpp"
#include "task/recovery_record.hpp"
#include "task/recovery_store.hpp"
#include "task/task_store.hpp"
#include "time/clock.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rodaq::ControlPrecondition;
using rodaq::DataImpact;
using rodaq::DesiredState;
using rodaq::ExecutionCondition;
using rodaq::OperationId;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::RecoveryPolicyMode;
using rodaq::StatusCode;
using rodaq::TaskConfig;
using rodaq::TaskId;
using rodaq::daq::Composition;
using rodaq::daq::DeploymentOpenMode;
using rodaq::daq::StartupOptions;

class BootReconciliationTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 48> pattern{};
        const std::string prefix = "/tmp/rodaq-boot-rec-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        fs::create_directory(root_ / "packages");
        fs::create_directory(root_ / "state");
        fs::create_directory(root_ / "volume");
        install_package(RODAQ_BOOT_STORAGE_PACKAGE, "rodaq.fake.storage");
        install_package(RODAQ_BOOT_ACQUISITION_PACKAGE, "rodaq.fake.acquisition");
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
        s.qualified_runtime_profile_id = "test-boot-reconcile";
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

    TaskConfig make_task_config(RecoveryPolicyMode policy, std::string name = "boot-task") {
        TaskConfig config;
        config.name = std::move(name);
        config.recording_mode = rodaq::RecordingMode::Continuous;
        config.start_policy = rodaq::StartPolicyMode::AllRequired;
        config.recovery_policy = policy;
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
        TaskId task, OperationId op, OperationState expected = OperationState::Succeeded) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto result = composition_->tasks().get_operation(task, op);
            if (result.ok() && rodaq::task::terminal(result.value().state)) {
                if (result.value().state != expected) {
                    ADD_FAILURE() << "op terminal mismatch: state="
                                  << static_cast<int>(result.value().state)
                                  << " status=" << static_cast<int>(result.value().status_code)
                                  << " impact=" << static_cast<int>(result.value().data_impact);
                }
                return result.value().state == expected;
            }
            (void)composition_->observe();
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    bool wait_condition(TaskId task, ExecutionCondition expected) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            (void)composition_->observe();
            auto snap = composition_->tasks().get_task(task);
            if (snap.ok() && snap.value().execution_condition == expected) {
                return true;
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

    void close_composition() {
        ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 5s).ok());
        composition_.reset();
    }

    fs::path root_;
    std::unique_ptr<Composition> composition_;
};

TEST_F(BootReconciliationTest, RunningWithResumePolicyAutoResumesAfterCleanShutdownDrain) {
    auto opened = Composition::open(make_startup(DeploymentOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId task = composition_->tasks()
                            .create_task(make_task_config(RecoveryPolicyMode::Resume), {"c1"})
                            .value();
    const auto start = composition_->tasks().start(task, {"s1"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(task, start.value()));
    ASSERT_TRUE(wait_records(task, 2));

    // Process Shutdown Drain keeps DesiredState=Running and generation=2 without fake Stop.
    close_composition();

    auto reopened = Composition::open(make_startup(DeploymentOpenMode::Existing));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());

    ASSERT_TRUE(wait_condition(task, ExecutionCondition::Active));
    ASSERT_TRUE(wait_records(task, 2));

    auto snap = composition_->tasks().get_task(task);
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(snap.value().desired_state, DesiredState::Running);
    EXPECT_EQ(snap.value().control_generation, 2u);
    EXPECT_EQ(composition_->tasks().list_operations(task, {}).value().items.size(), 1u);

    const auto stop = composition_->tasks().stop(task, {"stop-after-resume"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(task, stop.value()));

    auto artifacts = composition_->artifacts().query_artifacts({});
    ASSERT_TRUE(artifacts.ok());
    ASSERT_EQ(artifacts.value().items.size(), 2u);
    EXPECT_GT(
        artifacts.value().items[0].recording_range.begin_ns,
        0u);  // Second run continued cumulative recording timeline.
}

TEST_F(BootReconciliationTest, RunningWithManualPolicyStaysProtectedUntilExplicitStopAndStart) {
    auto opened = Composition::open(make_startup(DeploymentOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId task = composition_->tasks()
                            .create_task(make_task_config(RecoveryPolicyMode::Manual), {"c1"})
                            .value();
    const auto start = composition_->tasks().start(task, {"s1"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(task, start.value()));
    ASSERT_TRUE(wait_records(task, 2));
    close_composition();

    auto reopened = Composition::open(make_startup(DeploymentOpenMode::Existing));
    ASSERT_TRUE(reopened.ok());
    composition_ = std::move(reopened.value());

    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(composition_->observe().ok());

    auto snap = composition_->tasks().get_task(task);
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(snap.value().desired_state, DesiredState::Running);
    EXPECT_EQ(snap.value().execution_condition, ExecutionCondition::Protected);
    EXPECT_EQ(snap.value().control_generation, 2u);
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 1u);

    // Explicit Stop -> Idle, then explicit Start -> Active.
    const auto stop = composition_->tasks().stop(task, {"manual-stop"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(task, stop.value()));
    EXPECT_EQ(
        composition_->tasks().get_task(task).value().execution_condition, ExecutionCondition::Idle);

    const auto restart = composition_->tasks().start(task, {"manual-restart"}, {true, 3});
    ASSERT_TRUE(restart.ok());
    ASSERT_TRUE(wait_operation(task, restart.value()));
    ASSERT_TRUE(wait_records(task, 2));
}

TEST_F(BootReconciliationTest, AcceptedInProgressStartContinuesEvenUnderManualPolicy) {
    auto opened = Composition::open(make_startup(DeploymentOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId task = composition_->tasks()
                            .create_task(make_task_config(RecoveryPolicyMode::Manual), {"c1"})
                            .value();
    close_composition();

    // Simulate crash immediately after durable Accepted Start (before Prepare/Open completed).
    OperationId start_op;
    {
        auto store =
            rodaq::task::TaskStore::open(
                root_ / "state/service/tasks" / rodaq::to_string(task),
                root_ / "state/service/control", task, rodaq::task::TaskStoreMode::Existing, {})
                .value();
        auto control = store->load_control().value();
        start_op = rodaq::task::new_operation_id().value();
        auto digest =
            rodaq::task::lifecycle_request_digest(task, OperationKind::Start, {true, 1}).value();
        rodaq::task::StoredOperation stored;
        stored.request = {"crash-start"};
        stored.request_digest = digest;
        stored.snapshot.operation_id = start_op;
        stored.snapshot.kind = OperationKind::Start;
        stored.snapshot.state = OperationState::Accepted;
        stored.snapshot.control_generation = 2;
        stored.snapshot.accepted_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        control.desired_state = DesiredState::Running;
        control.control_generation = 2;
        control.record_revision = 2;
        control.current_operation_id = start_op;
        control.operation_history.entries.push_back(stored);
        ASSERT_TRUE(store->publish_control(control, 1).ok());
    }

    auto reopened = Composition::open(make_startup(DeploymentOpenMode::Existing));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());

    // Manual policy MUST NOT cancel an Accepted/InProgress Start!
    ASSERT_TRUE(wait_operation(task, start_op, OperationState::Succeeded));
    ASSERT_TRUE(wait_condition(task, ExecutionCondition::Active));
    ASSERT_TRUE(wait_records(task, 2));

    const auto stop = composition_->tasks().stop(task, {"stop-continued"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(task, stop.value()));
}

TEST_F(
    BootReconciliationTest,
    OpenAuthorizedWithoutOpenedObservedStillResumesUnderResumePolicyAndProtectsUnderManual) {
    auto opened = Composition::open(make_startup(DeploymentOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId task = composition_->tasks()
                            .create_task(make_task_config(RecoveryPolicyMode::Resume), {"c1"})
                            .value();
    const auto start = composition_->tasks().start(task, {"s1"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(task, start.value()));
    ASSERT_TRUE(wait_records(task, 1));
    close_composition();

    // Rewrite Recovery record to simulate a crash right after OpenAuthorized with
    // opened_observed=false (before Ended was written).
    {
        auto rec_store =
            rodaq::task::TaskRecoveryStore::open(root_ / "state/service/recovery", task, {})
                .value();
        auto rec = rec_store->load().value();
        ASSERT_TRUE(rec.run_operation.has_value());
        rec.run_operation->phase = rodaq::task::RunPhase::OpenAuthorized;
        for (auto& pipe : rec.run_operation->authorized_pipelines) {
            pipe.opened_observed = false;
            pipe.ended_boundary.reset();
            pipe.continuation_permission = true;
        }
        auto bytes = rodaq::task::encode_recovery_record(rec, {}).value();
        auto pub = rodaq::persistence::publish_file(
            root_ / "state/service/recovery", rodaq::to_string(task) + ".json", bytes,
            rodaq::persistence::FilePublishMode::Replace, 256 * 1024);
        ASSERT_TRUE(pub.status.ok());
    }

    auto reopened = Composition::open(make_startup(DeploymentOpenMode::Existing));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());

    // Missing opened_observed MUST NOT overturn durable OpenAuthorized.
    ASSERT_TRUE(wait_condition(task, ExecutionCondition::Active));
    ASSERT_TRUE(wait_records(task, 2));

    const auto stop = composition_->tasks().stop(task, {"stop-oa"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(task, stop.value()));
}

TEST_F(BootReconciliationTest, MissingExactPluginVersionStaysProtectedAndNeverFallsBack) {
    auto opened = Composition::open(make_startup(DeploymentOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId task = composition_->tasks()
                            .create_task(make_task_config(RecoveryPolicyMode::Resume), {"c1"})
                            .value();
    const auto start = composition_->tasks().start(task, {"s1"}, {true, 1});
    ASSERT_TRUE(start.ok());
    ASSERT_TRUE(wait_operation(task, start.value()));
    ASSERT_TRUE(wait_records(task, 1));
    close_composition();

    // Reopen with the exact acquisition plugin version 1.0.0 no longer allowed
    // (only a different version 1.0.1 is admitted). Must stay Protected, never fallback.
    auto opts = make_startup(DeploymentOpenMode::Existing);
    opts.settings.allowed_plugins = {
        {"rodaq.fake.acquisition", "1.0.1", rodaq::task::AdmissionRole::Acquisition},
        {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
    auto reopened = Composition::open(std::move(opts));
    ASSERT_TRUE(reopened.ok());
    composition_ = std::move(reopened.value());

    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(composition_->observe().ok());

    auto snap = composition_->tasks().get_task(task);
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(snap.value().desired_state, DesiredState::Running);
    EXPECT_EQ(snap.value().execution_condition, ExecutionCondition::Protected);

    // Explicit Stop still settles to Idle.
    const auto stop = composition_->tasks().stop(task, {"stop-incompat"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(task, stop.value()));
    EXPECT_EQ(
        composition_->tasks().get_task(task).value().execution_condition, ExecutionCondition::Idle);
}

TEST_F(BootReconciliationTest, PausedTaskNeverAutoOpensAndIncompleteStopSettlesFailedWithImpact) {
    auto opened = Composition::open(make_startup(DeploymentOpenMode::Initialize));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId paused_task =
        composition_->tasks()
            .create_task(make_task_config(RecoveryPolicyMode::Resume, "p"), {"c1"})
            .value();
    const auto s1 = composition_->tasks().start(paused_task, {"s1"}, {true, 1});
    ASSERT_TRUE(s1.ok());
    ASSERT_TRUE(wait_operation(paused_task, s1.value()));
    ASSERT_TRUE(wait_records(paused_task, 1));
    const auto p1 = composition_->tasks().pause(paused_task, {"p1"}, {true, 2});
    ASSERT_TRUE(p1.ok());
    ASSERT_TRUE(wait_operation(paused_task, p1.value()));

    const TaskId stop_task =
        composition_->tasks()
            .create_task(make_task_config(RecoveryPolicyMode::Resume, "s"), {"c2"})
            .value();
    const auto s2 = composition_->tasks().start(stop_task, {"s2"}, {true, 1});
    ASSERT_TRUE(s2.ok());
    ASSERT_TRUE(wait_operation(stop_task, s2.value()));
    ASSERT_TRUE(wait_records(stop_task, 1));
    close_composition();

    // Simulate crash on stop_task while Stop was Accepted and Recovery was still OpenAuthorized.
    OperationId stop_op;
    {
        auto store = rodaq::task::TaskStore::open(
                         root_ / "state/service/tasks" / rodaq::to_string(stop_task),
                         root_ / "state/service/control", stop_task,
                         rodaq::task::TaskStoreMode::Existing, {})
                         .value();
        auto control = store->load_control().value();
        stop_op = rodaq::task::new_operation_id().value();
        auto digest =
            rodaq::task::lifecycle_request_digest(stop_task, OperationKind::Stop, {true, 2})
                .value();
        rodaq::task::StoredOperation stored;
        stored.request = {"crash-stop"};
        stored.request_digest = digest;
        stored.snapshot.operation_id = stop_op;
        stored.snapshot.kind = OperationKind::Stop;
        stored.snapshot.state = OperationState::Accepted;
        stored.snapshot.control_generation = 3;
        stored.snapshot.accepted_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        control.desired_state = DesiredState::Stopped;
        control.control_generation = 3;
        control.record_revision += 1;
        control.current_operation_id = stop_op;
        control.operation_history.entries.push_back(stored);
        ASSERT_TRUE(store->publish_control(control, control.record_revision - 1).ok());

        auto rec_store =
            rodaq::task::TaskRecoveryStore::open(root_ / "state/service/recovery", stop_task, {})
                .value();
        auto rec = rec_store->load().value();
        rec.run_operation->phase = rodaq::task::RunPhase::OpenAuthorized;
        for (auto& pipe : rec.run_operation->authorized_pipelines) {
            pipe.ended_boundary.reset();
        }
        auto bytes = rodaq::task::encode_recovery_record(rec, {}).value();
        ASSERT_TRUE(rodaq::persistence::publish_file(
                        root_ / "state/service/recovery", rodaq::to_string(stop_task) + ".json",
                        bytes, rodaq::persistence::FilePublishMode::Replace, 256 * 1024)
                        .status.ok());
    }

    auto reopened = Composition::open(make_startup(DeploymentOpenMode::Existing));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());

    // Paused task stays Paused + Idle and does NOT auto-open.
    auto psnap = composition_->tasks().get_task(paused_task).value();
    EXPECT_EQ(psnap.desired_state, DesiredState::Paused);
    EXPECT_EQ(psnap.execution_condition, ExecutionCondition::Idle);

    // Incomplete Stop during OpenAuthorized settles to Failed + PersistenceUnknown, Stopped + Idle.
    auto ssnap = composition_->tasks().get_task(stop_task).value();
    EXPECT_EQ(ssnap.desired_state, DesiredState::Stopped);
    EXPECT_EQ(ssnap.execution_condition, ExecutionCondition::Idle);
    auto op_snap = composition_->tasks().get_operation(stop_task, stop_op).value();
    EXPECT_EQ(op_snap.state, OperationState::Failed);
    EXPECT_EQ(op_snap.data_impact, DataImpact::PersistenceUnknown);
}

TEST_F(BootReconciliationTest, ResetCrashResumesAfterRestartAndTombstoneGcWaitsForArtifactGc) {
    auto startup = make_startup(DeploymentOpenMode::Initialize);
    startup.settings.artifact_audit_window = 24h;
    auto opened = Composition::open(std::move(startup));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    const TaskId task = composition_->tasks()
                            .create_task(make_task_config(RecoveryPolicyMode::Manual), {"c1"})
                            .value();
    const auto s1 = composition_->tasks().start(task, {"s1"}, {true, 1});
    ASSERT_TRUE(s1.ok());
    ASSERT_TRUE(wait_operation(task, s1.value()));
    ASSERT_TRUE(wait_records(task, 1));
    const auto stop = composition_->tasks().stop(task, {"stop1"}, {true, 2});
    ASSERT_TRUE(stop.ok());
    ASSERT_TRUE(wait_operation(task, stop.value()));
    ASSERT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 1u);
    close_composition();

    // Simulate crash immediately after durable Reset intent was published (before delete ran).
    OperationId reset_op;
    {
        auto store =
            rodaq::task::TaskStore::open(
                root_ / "state/service/tasks" / rodaq::to_string(task),
                root_ / "state/service/control", task, rodaq::task::TaskStoreMode::Existing, {})
                .value();
        auto control = store->load_control().value();
        reset_op = rodaq::task::new_operation_id().value();
        auto digest =
            rodaq::task::lifecycle_request_digest(task, OperationKind::Reset, {true, 3}).value();
        rodaq::task::StoredOperation stored;
        stored.request = {"crash-reset"};
        stored.request_digest = digest;
        stored.snapshot.operation_id = reset_op;
        stored.snapshot.kind = OperationKind::Reset;
        stored.snapshot.state = OperationState::Accepted;
        stored.snapshot.control_generation = 4;
        stored.snapshot.accepted_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        control.control_generation = 4;
        control.reset_generation = 1;
        control.reset_intent = rodaq::task::ResetIntent{reset_op, 1, 3};
        control.record_revision += 1;
        control.current_operation_id = reset_op;
        control.operation_history.entries.push_back(stored);
        ASSERT_TRUE(store->publish_control(control, control.record_revision - 1).ok());
    }

    // Reopen -> boot reconciliation automatically resumes Reset and deletes the artifact!
    auto reopen_opts = make_startup(DeploymentOpenMode::Existing);
    reopen_opts.settings.artifact_audit_window = 24h;
    auto reopened = Composition::open(std::move(reopen_opts));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());

    ASSERT_TRUE(wait_operation(task, reset_op, OperationState::Succeeded));
    EXPECT_TRUE(composition_->artifacts().query_artifacts({}).value().items.empty());
    EXPECT_EQ(
        composition_->tasks().get_task(task).value().execution_condition, ExecutionCondition::Idle);

    // Retire the Task while artifact Deleted tombstone is STILL retained (24h audit window).
    const auto retire = composition_->tasks().retire(task, {"retire-after-reset"}, {true, 4});
    ASSERT_TRUE(retire.ok());
    ASSERT_TRUE(wait_operation(task, retire.value(), OperationState::Succeeded));

    // Tombstone MUST NOT be GC'd while artifact Deleted tombstone still references Task!
    ASSERT_TRUE(composition_->observe().ok());
    EXPECT_TRUE(fs::exists(root_ / "state/service/retired" / (rodaq::to_string(task) + ".json")));
    EXPECT_TRUE(composition_->tasks().get_operation(task, retire.value()).ok());
}

}  // namespace
