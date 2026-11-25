#include "daq/startup.hpp"
#include "task/config.hpp"
#include "task/control_record.hpp"
#include "task/recovery_store.hpp"
#include "task/retirement_store.hpp"
#include "task/task_store.hpp"
#include "pipeline/stream_identity_store.hpp"
#include "service_config.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
namespace daq = rodaq::daq;
using namespace std::chrono_literals;
using rodaq::ExecutionCondition;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::StatusCode;

class S4cClosureTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-s4c-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        state_root_ = root_ / "state";
        volume_root_ = root_ / "volume";
        packages_root_ = root_ / "packages";
        fs::create_directory(state_root_);
        fs::create_directory(volume_root_);
        fs::create_directory(packages_root_);
        fs::copy(
            RODAQ_S4C_STORAGE_PACKAGE, packages_root_ / "storage", fs::copy_options::recursive);
        fs::copy(
            RODAQ_S4C_ACQUISITION_PACKAGE, packages_root_ / "acquisition",
            fs::copy_options::recursive);
        for (const auto& entry : fs::recursive_directory_iterator(packages_root_)) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(packages_root_, fs::perms::owner_read | fs::perms::owner_exec);
    }

    void TearDown() override {
        if (composition_) {
            (void)composition_->shutdown(std::chrono::steady_clock::now() + 5s);
            composition_.reset();
        }
        if (!root_.empty()) {
            std::error_code ec;
            fs::permissions(packages_root_, fs::perms::owner_all, ec);
            for (const auto& entry : fs::recursive_directory_iterator(packages_root_, ec)) {
                fs::permissions(entry.path(), fs::perms::owner_all, ec);
            }
            fs::remove_all(root_, ec);
        }
    }

    daq::StartupOptions startup_options(
        daq::DeploymentOpenMode mode, std::size_t max_tasks = 2,
        std::chrono::nanoseconds audit_window = 10ms) const {
        daq::StartupOptions opts;
        opts.state_root = state_root_;
        opts.volume_root = volume_root_;
        opts.mode = mode;
        opts.plugins.search_paths = {packages_root_};
        opts.plugins.max_loaded_plugins = 4;
        opts.plugins.max_instances = 16;
        opts.plugins.max_config_size = 4096;
        opts.plugins.shutdown_timeout = 5s;
        opts.capacity.max_managed_bytes = 64ull * 1024 * 1024;
        opts.capacity.max_commit_bytes = 16ull * 1024 * 1024;
        opts.capacity.stop_free_bytes = 1024;
        opts.capacity.low_free_bytes = 2048;
        opts.capacity.recovery_free_bytes = 4096;
        opts.capacity.recovery_stable_ns = 1;
        opts.capacity.space_sample_max_age_ns = 60'000'000'000ull;
        opts.ledger_records = 64;
        opts.settings.deployment_generation = 1;
        opts.settings.qualified_runtime_profile_id = "s4c-test-profile";
        opts.settings.output_root = "local";
        opts.settings.max_tasks = max_tasks;
        opts.settings.artifact_audit_window = audit_window;
        daq::ResourceProfile profile;
        profile.name = "basic";
        profile.instance.records = 64;
        profile.instance.logical_bytes = 4096;
        profile.instance.payloads = {65536, 4096, 256, 64};
        profile.instance.queue_capacity = 32;
        profile.instance.callback_concurrency = 2;
        profile.storage.header.struct_size = sizeof(rodaq::storage::PrepareConfig);
        profile.storage.max_record_bytes = 256;
        profile.storage.max_batch_bytes = 4096;
        profile.storage.max_batch_records = 16;
        profile.storage.max_descriptors_per_segment = 32;
        profile.storage.max_segment_records = 10000;
        profile.storage.max_segment_input_bytes = 2 * 1024 * 1024;
        profile.storage.max_segment_physical_bytes = 4 * 1024 * 1024;
        profile.storage.max_segment_file_count = 8;
        profile.storage.max_relative_path_bytes = 128;
        opts.settings.profiles.push_back(profile);
        opts.settings.allowed_plugins = {
            {"rodaq.fake.acquisition", "1.0.0", rodaq::task::AdmissionRole::Acquisition},
            {"rodaq.fake.storage", "1.0.0", rodaq::task::AdmissionRole::Storage}};
        opts.settings.storage = {{"rodaq.fake.storage", "1.0.0", "fake"}};
        return opts;
    }

    rodaq::TaskConfig make_config(
        rodaq::RecoveryPolicyMode recovery_policy = rodaq::RecoveryPolicyMode::Manual) const {
        auto config = rodaq::test::service_config().value();
        config.recovery_policy = recovery_policy;
        config.pipelines[0].acquisition = {
            "rodaq.fake.acquisition", "1.0.0", R"({"streams":["sample"]})"};
        config.pipelines[0].streams[0].stream_key = "sample";
        config.pipelines[0].storage = {"rodaq.fake.storage", "1.0.0", "{}"};
        return config;
    }

    bool wait_operation(rodaq::TaskId task_id, rodaq::OperationId op_id, OperationState expected) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto snap = composition_->tasks().get_operation(task_id, op_id);
            if (snap.ok()) {
                if (snap.value().state == expected) {
                    return true;
                }
                if (snap.value().state == OperationState::Succeeded ||
                    snap.value().state == OperationState::Failed) {
                    ADD_FAILURE() << "Operation ended in state "
                                  << static_cast<int>(snap.value().state)
                                  << " status=" << static_cast<int>(snap.value().status_code)
                                  << " msg=" << snap.value().diagnostic;
                    return false;
                }
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    bool wait_artifacts(std::size_t expected_count) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto page = composition_->artifacts().query_artifacts({});
            if (page.ok() && page.value().items.size() == expected_count) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    bool wait_condition(rodaq::TaskId task_id, ExecutionCondition expected) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto snap = composition_->tasks().get_task(task_id);
            if (snap.ok() && snap.value().execution_condition == expected) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    fs::path root_;
    fs::path state_root_;
    fs::path volume_root_;
    fs::path packages_root_;
    std::unique_ptr<daq::Composition> composition_;
};

TEST_F(S4cClosureTest, ResetDeletesArtifactsPreservesHighWaterAndAllowsRetireWithNonResurrection) {
    auto opened = daq::Composition::open(startup_options(daq::DeploymentOpenMode::Initialize, 1));
    ASSERT_TRUE(opened.ok()) << static_cast<int>(opened.status().code());
    composition_ = std::move(opened.value());
    ASSERT_TRUE(composition_->observe().ok());

    // Verify SystemService snapshot & capabilities.
    auto sys = composition_->system().snapshot();
    ASSERT_TRUE(sys.ok());
    EXPECT_TRUE(sys.value().core_ready);
    EXPECT_TRUE(sys.value().recovery.complete);
    ASSERT_EQ(sys.value().volumes.size(), 1u);
    EXPECT_EQ(sys.value().volumes[0].name, "local");
    EXPECT_GT(sys.value().resources.threads, 0u);
    auto caps = composition_->system().capabilities();
    ASSERT_TRUE(caps.ok());
    EXPECT_EQ(caps.value().application_version, "0.1.0");
    EXPECT_GE(caps.value().backends.size(), 2u);

    auto created = composition_->tasks().create_task(make_config(), {"create-1"});
    ASSERT_TRUE(created.ok());
    const auto task_id = created.value();

    // Start, record, and Stop to produce a committed artifact.
    auto start_op = composition_->tasks().start(task_id, {"start-1"}, {});
    ASSERT_TRUE(start_op.ok());
    ASSERT_TRUE(wait_operation(task_id, start_op.value(), OperationState::Succeeded));
    std::this_thread::sleep_for(80ms);
    auto stop_op = composition_->tasks().stop(task_id, {"stop-1"}, {});
    ASSERT_TRUE(stop_op.ok());
    ASSERT_TRUE(wait_operation(task_id, stop_op.value(), OperationState::Succeeded));
    ASSERT_TRUE(wait_artifacts(1));

    // Retire MUST be rejected while a committed artifact exists.
    auto blocked_retire = composition_->tasks().retire(task_id, {"retire-blocked"}, {});
    EXPECT_FALSE(blocked_retire.ok());
    EXPECT_EQ(composition_->artifacts().query_artifacts({}).value().items.size(), 1u);

    // Reset with stale generation fails with Conflict.
    EXPECT_EQ(
        composition_->tasks().reset(task_id, {"reset-stale"}, {true, 1}).status().code(),
        StatusCode::Conflict);

    const auto gen_before_reset =
        composition_->tasks().get_task(task_id).value().control_generation;
    auto reset_op = composition_->tasks().reset(task_id, {"reset-1"}, {true, gen_before_reset});
    ASSERT_TRUE(reset_op.ok());
    // Idempotent retry returns the exact same OperationId.
    auto reset_retry = composition_->tasks().reset(task_id, {"reset-1"}, {true, gen_before_reset});
    ASSERT_TRUE(reset_retry.ok());
    EXPECT_EQ(reset_retry.value(), reset_op.value());
    // Same key + different precondition returns Conflict.
    EXPECT_EQ(
        composition_->tasks().reset(task_id, {"reset-1"}, {true, 999}).status().code(),
        StatusCode::Conflict);

    ASSERT_TRUE(wait_operation(task_id, reset_op.value(), OperationState::Succeeded));
    ASSERT_TRUE(wait_artifacts(0));
    EXPECT_EQ(composition_->tasks().get_task(task_id).value().reset_generation, 1u);

    // Verify StreamIdentity high-water was NOT rolled back by Reset.
    {
        std::ifstream id_file(
            state_root_ / "service/identities" / (rodaq::to_string(task_id) + ".json"));
        ASSERT_TRUE(id_file.good());
        const auto id_json = rodaq::persistence::Json::parse(id_file);
        ASSERT_TRUE(id_json.contains("streams"));
        ASSERT_FALSE(id_json["streams"].empty());
        const auto reserved_end =
            std::stoull(id_json["streams"][0]["reserved_end_exclusive"].get<std::string>());
        EXPECT_GT(reserved_end, 1u);
    }

    // Now Retire succeeds (only Deleted tombstone remains) and frees max_tasks=1 slot!
    const auto gen_before_retire =
        composition_->tasks().get_task(task_id).value().control_generation;
    auto retire_op = composition_->tasks().retire(task_id, {"retire-1"}, {true, gen_before_retire});
    ASSERT_TRUE(retire_op.ok());
    EXPECT_EQ(composition_->tasks().get_task(task_id).status().code(), StatusCode::NotFound);
    EXPECT_TRUE(composition_->tasks().list_tasks({}).value().items.empty());

    // Operation query and idempotent Retire retry succeed from durable tombstone.
    auto op_snap = composition_->tasks().get_operation(task_id, retire_op.value());
    ASSERT_TRUE(op_snap.ok());
    EXPECT_EQ(op_snap.value().kind, OperationKind::Retire);
    EXPECT_EQ(op_snap.value().state, OperationState::Succeeded);
    auto retire_retry =
        composition_->tasks().retire(task_id, {"retire-1"}, {true, gen_before_retire});
    ASSERT_TRUE(retire_retry.ok());
    EXPECT_EQ(retire_retry.value(), retire_op.value());

    // Freed active Task slot allows creating a new Task at max_tasks=1.
    auto created2 = composition_->tasks().create_task(make_config(), {"create-2"});
    ASSERT_TRUE(created2.ok());
    EXPECT_FALSE(created2.value() == task_id);

    // Restart and prove retired Task NEVER resurrects.
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 5s).ok());
    composition_.reset();

    auto reopened = daq::Composition::open(startup_options(daq::DeploymentOpenMode::Existing, 1));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());
    EXPECT_EQ(composition_->tasks().get_task(task_id).status().code(), StatusCode::NotFound);
    EXPECT_EQ(composition_->tasks().list_tasks({}).value().items.size(), 1u);
    auto op_after_boot = composition_->tasks().get_operation(task_id, retire_op.value());
    ASSERT_TRUE(op_after_boot.ok());
    EXPECT_EQ(op_after_boot.value().state, OperationState::Succeeded);
}

TEST_F(S4cClosureTest, BootReconciliationRespectsManualAndResumeRecoveryPolicies) {
    auto opened = daq::Composition::open(startup_options(daq::DeploymentOpenMode::Initialize, 2));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());
    ASSERT_TRUE(composition_->observe().ok());

    auto manual_id =
        composition_->tasks().create_task(make_config(rodaq::RecoveryPolicyMode::Manual), {"m1"});
    auto resume_id =
        composition_->tasks().create_task(make_config(rodaq::RecoveryPolicyMode::Resume), {"r1"});
    ASSERT_TRUE(manual_id.ok());
    ASSERT_TRUE(resume_id.ok());

    auto m_start = composition_->tasks().start(manual_id.value(), {"ms1"}, {});
    auto r_start = composition_->tasks().start(resume_id.value(), {"rs1"}, {});
    ASSERT_TRUE(m_start.ok());
    ASSERT_TRUE(r_start.ok());
    ASSERT_TRUE(wait_operation(manual_id.value(), m_start.value(), OperationState::Succeeded));
    ASSERT_TRUE(wait_operation(resume_id.value(), r_start.value(), OperationState::Succeeded));

    // Process shutdown drains WITHOUT changing durable DesiredState (still Running!).
    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 5s).ok());
    composition_.reset();

    auto reopened = daq::Composition::open(startup_options(daq::DeploymentOpenMode::Existing, 2));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());

    // Manual task MUST remain Protected (Gate Closed), while operator can Stop it cleanly.
    EXPECT_EQ(
        composition_->tasks().get_task(manual_id.value()).value().execution_condition,
        ExecutionCondition::Protected);
    auto m_stop = composition_->tasks().stop(manual_id.value(), {"m-stop-after-boot"}, {});
    ASSERT_TRUE(m_stop.ok());
    ASSERT_TRUE(wait_operation(manual_id.value(), m_stop.value(), OperationState::Succeeded));
    EXPECT_EQ(
        composition_->tasks().get_task(manual_id.value()).value().execution_condition,
        ExecutionCondition::Idle);

    // Resume task MUST automatically re-authorize and become Active!
    EXPECT_TRUE(wait_condition(resume_id.value(), ExecutionCondition::Active));
}

TEST_F(S4cClosureTest, InterruptedResetResumesDeterministicallyOnBoot) {
    auto opened = daq::Composition::open(startup_options(daq::DeploymentOpenMode::Initialize, 1));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());
    ASSERT_TRUE(composition_->observe().ok());

    auto created = composition_->tasks().create_task(make_config(), {"create-reset-boot"});
    ASSERT_TRUE(created.ok());
    const auto task_id = created.value();
    auto start_op = composition_->tasks().start(task_id, {"s1"}, {});
    ASSERT_TRUE(start_op.ok());
    ASSERT_TRUE(wait_operation(task_id, start_op.value(), OperationState::Succeeded));
    std::this_thread::sleep_for(60ms);
    auto stop_op = composition_->tasks().stop(task_id, {"st1"}, {});
    ASSERT_TRUE(stop_op.ok());
    ASSERT_TRUE(wait_operation(task_id, stop_op.value(), OperationState::Succeeded));
    ASSERT_TRUE(wait_artifacts(1));

    ASSERT_TRUE(composition_->shutdown(std::chrono::steady_clock::now() + 5s).ok());
    composition_.reset();

    // Inject a durable ResetIntent as if the process crashed immediately after Reset acceptance.
    rodaq::OperationId injected_reset_op{};
    {
        auto store = rodaq::task::TaskStore::open(
            state_root_ / "service/tasks" / rodaq::to_string(task_id),
            state_root_ / "service/control", task_id, rodaq::task::TaskStoreMode::Existing, {});
        ASSERT_TRUE(store.ok());
        auto control = store.value()->load_control();
        ASSERT_TRUE(control.ok());
        auto ctrl = control.value();
        const auto old_rev = ctrl.record_revision;
        const auto target_gen = ctrl.control_generation;
        ++ctrl.control_generation;
        ++ctrl.reset_generation;
        ++ctrl.record_revision;
        injected_reset_op.value[0] = 0x77;
        rodaq::task::StoredOperation stored_op;
        stored_op.request.request_key = "injected-reset";
        stored_op.request_digest =
            "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        stored_op.snapshot.operation_id = injected_reset_op;
        stored_op.snapshot.kind = OperationKind::Reset;
        stored_op.snapshot.state = OperationState::Accepted;
        stored_op.snapshot.control_generation = ctrl.control_generation;
        stored_op.snapshot.accepted_at = std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now());
        ctrl.operation_history.entries.push_back(stored_op);
        ctrl.current_operation_id = injected_reset_op;
        ctrl.reset_intent =
            rodaq::task::ResetIntent{injected_reset_op, ctrl.reset_generation, target_gen};
        ASSERT_TRUE(store.value()->publish_control(ctrl, old_rev).ok());
    }

    auto reopened = daq::Composition::open(startup_options(daq::DeploymentOpenMode::Existing, 1));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());
    ASSERT_TRUE(wait_operation(task_id, injected_reset_op, OperationState::Succeeded));
    ASSERT_TRUE(wait_artifacts(0));
    EXPECT_EQ(
        composition_->tasks().get_task(task_id).value().execution_condition,
        ExecutionCondition::Idle);
}

TEST_F(S4cClosureTest, RealCrashWhileRunningAndQuarantinePresentReconcileCleanlyOnBoot) {
    // Initialize in a child process, start a Resume task, and simulate an abrupt crash (_exit).
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        auto opened =
            daq::Composition::open(startup_options(daq::DeploymentOpenMode::Initialize, 1));
        if (!opened.ok()) {
            ::_exit(10);
        }
        auto comp = std::move(opened.value());
        if (!comp->observe().ok()) {
            ::_exit(11);
        }
        auto created =
            comp->tasks().create_task(make_config(rodaq::RecoveryPolicyMode::Resume), {"c1"});
        if (!created.ok()) {
            ::_exit(12);
        }
        auto start_op = comp->tasks().start(created.value(), {"s1"}, {});
        if (!start_op.ok()) {
            ::_exit(13);
        }
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        bool started = false;
        while (std::chrono::steady_clock::now() < deadline) {
            auto snap = comp->tasks().get_operation(created.value(), start_op.value());
            if (snap.ok() && snap.value().state == OperationState::Succeeded) {
                started = true;
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        if (!started) {
            ::_exit(14);
        }
        std::this_thread::sleep_for(40ms);
        ::_exit(0);  // Abrupt process termination without shutdown().
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    // Inject a quarantined entry on the Volume to verify non-blocking startup + SystemService
    // telemetry.
    const auto q_dir = volume_root_ / ".rodaq/quarantine/999.precommit.tmp";
    fs::create_directories(q_dir);
    {
        std::ofstream payload(q_dir / "partial.bin", std::ios::binary);
        payload << std::string(1024, 'Q');
    }

    auto reopened = daq::Composition::open(startup_options(daq::DeploymentOpenMode::Existing, 1));
    ASSERT_TRUE(reopened.ok()) << static_cast<int>(reopened.status().code());
    composition_ = std::move(reopened.value());

    const auto sys = composition_->system().snapshot();
    ASSERT_TRUE(sys.ok());
    EXPECT_TRUE(sys.value().core_ready);
    EXPECT_GE(sys.value().backlog.quarantined_items, 1u);
    ASSERT_EQ(sys.value().volumes.size(), 1u);
    EXPECT_GE(sys.value().volumes[0].quarantined_bytes, 1024u);

    const auto listed = composition_->tasks().list_tasks({});
    ASSERT_TRUE(listed.ok());
    ASSERT_EQ(listed.value().items.size(), 1u);
    EXPECT_TRUE(wait_condition(listed.value().items[0].task.task_id, ExecutionCondition::Active));
}

TEST_F(S4cClosureTest, RetiredTombstoneGarbageCollectsAfterAuditWindowWithZeroReferences) {
    auto opened =
        daq::Composition::open(startup_options(daq::DeploymentOpenMode::Initialize, 1, 15ms));
    ASSERT_TRUE(opened.ok());
    composition_ = std::move(opened.value());

    auto created = composition_->tasks().create_task(make_config(), {"gc-create"});
    ASSERT_TRUE(created.ok());
    const auto task_id = created.value();
    const auto gen = composition_->tasks().get_task(task_id).value().control_generation;

    auto retire_op = composition_->tasks().retire(task_id, {"gc-retire"}, {true, gen});
    ASSERT_TRUE(retire_op.ok());
    ASSERT_TRUE(composition_->tasks().get_operation(task_id, retire_op.value()).ok());

    // Wait past the 15ms audit window and trigger observe() maintenance collection.
    std::this_thread::sleep_for(30ms);
    ASSERT_TRUE(composition_->observe().ok());

    // Tombstone is collected once audit window has elapsed and zero artifact references exist.
    EXPECT_EQ(
        composition_->tasks().get_operation(task_id, retire_op.value()).status().code(),
        StatusCode::NotFound);
    EXPECT_FALSE(
        fs::exists(state_root_ / "service/retired" / (rodaq::to_string(task_id) + ".json")));
}

}  // namespace
