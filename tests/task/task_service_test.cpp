#include "service_config.hpp"
#include "task/create_store.hpp"
#include "task/identity.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <future>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
namespace fs = std::filesystem;
namespace task = rodaq::task;
using rodaq::StatusCode;
using namespace std::chrono_literals;

class TaskServiceTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-service-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        ASSERT_TRUE(fs::create_directory(root_ / "state"));
        ASSERT_TRUE(fs::create_directory(root_ / "packages"));
        fs::copy(
            RODAQ_SERVICE_ACQUISITION_PACKAGE, root_ / "packages/acquisition",
            fs::copy_options::recursive);
        fs::copy(
            RODAQ_SERVICE_STORAGE_PACKAGE, root_ / "packages/storage", fs::copy_options::recursive);
        for (const auto& entry : fs::recursive_directory_iterator(root_ / "packages")) {
            fs::permissions(entry.path(), fs::perms::owner_read | fs::perms::owner_exec);
        }
        fs::permissions(root_ / "packages", fs::perms::owner_read | fs::perms::owner_exec);
        auto manager = rodaq::test::service_plugins(root_ / "packages");
        ASSERT_TRUE(manager.ok());
        manager_ = std::move(manager.value());
        auto lane = rodaq::ExecutionLane::create(1, 8);
        ASSERT_TRUE(lane.ok());
        lane_ = std::move(lane.value());
        auto control = rodaq::ExecutionLane::create(2, 8);
        ASSERT_TRUE(control.ok());
        control_lane_ = std::move(control.value());
        options_ = rodaq::test::service_options(root_ / "state", manager_, lane_, control_lane_);
        auto config = rodaq::test::service_config();
        ASSERT_TRUE(config.ok());
        config_ = std::move(config.value());
    }

    void TearDown() override {
        if (lane_) {
            EXPECT_TRUE(lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
        }
        if (control_lane_) {
            EXPECT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
        }
        service_.reset();
        if (manager_) {
            EXPECT_TRUE(manager_->shutdown(rodaq::plugin::Deadline::after(2s).value()).ok());
        }
        manager_.reset();
        std::error_code ignored;
        for (const auto& entry : fs::recursive_directory_iterator(root_, ignored)) {
            fs::permissions(entry.path(), fs::perms::owner_all, ignored);
        }
        fs::remove_all(root_, ignored);
    }

    void open() {
        auto service = rodaq::TaskService::open(options_);
        ASSERT_TRUE(service.ok()) << static_cast<int>(service.status().code());
        service_ = std::move(service.value());
    }

    void restart() {
        service_.reset();
        ASSERT_TRUE(lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
        ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
        auto next = rodaq::ExecutionLane::create(1, 8);
        ASSERT_TRUE(next.ok());
        lane_ = std::move(next.value());
        options_.create_lane = lane_;
        auto control = rodaq::ExecutionLane::create(2, 8);
        ASSERT_TRUE(control.ok());
        control_lane_ = std::move(control.value());
        options_.control_lane = control_lane_;
        open();
    }

    bool crash_at(int stage_value, const char* mode = nullptr) {
        struct Child {
            pid_t pid{-1};
            ~Child() {
                if (pid > 0) {
                    (void)kill(pid, SIGKILL);
                    int status = 0;
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                    }
                }
            }
        } child;
        int pipe_fds[2];
        const auto piped = pipe(pipe_fds);
        EXPECT_EQ(piped, 0);
        if (piped != 0) {
            return false;
        }
        const std::string root = (root_ / "state").string();
        const std::string packages = (root_ / "packages").string();
        const auto stage = std::to_string(stage_value);
        const auto fd = std::to_string(pipe_fds[1]);
        const char* arguments[] = {
            RODAQ_SERVICE_CRASH_PROBE,
            root.c_str(),
            packages.c_str(),
            stage.c_str(),
            fd.c_str(),
            mode,
            nullptr};
        const auto spawned = posix_spawn(
            &child.pid, RODAQ_SERVICE_CRASH_PROBE, nullptr, nullptr,
            const_cast<char* const*>(arguments), environ);
        close(pipe_fds[1]);
        EXPECT_EQ(spawned, 0);
        if (spawned != 0) {
            close(pipe_fds[0]);
            return false;
        }
        pollfd observed{pipe_fds[0], POLLIN, 0};
        const auto ready = poll(&observed, 1, 5000);
        char entered = 0;
        const auto read_count = ready > 0 ? read(pipe_fds[0], &entered, 1) : -1;
        close(pipe_fds[0]);
        EXPECT_EQ(read_count, 1);
        if (read_count != 1) {
            return false;
        }
        EXPECT_EQ(entered, 'x');
        EXPECT_EQ(kill(child.pid, 0), 0);
        EXPECT_EQ(kill(child.pid, SIGKILL), 0);
        int status = 0;
        EXPECT_EQ(waitpid(child.pid, &status, 0), child.pid);
        child.pid = -1;
        EXPECT_TRUE(WIFSIGNALED(status));
        EXPECT_EQ(WTERMSIG(status), SIGKILL);
        return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    }

    std::size_t task_directories() {
        return static_cast<std::size_t>(
            std::distance(fs::directory_iterator(root_ / "state/tasks"), fs::directory_iterator{}));
    }

    fs::path root_;
    std::shared_ptr<rodaq::plugin::PluginManager> manager_;
    std::shared_ptr<rodaq::ExecutionLane> lane_;
    std::shared_ptr<rodaq::ExecutionLane> control_lane_;
    task::ServiceOptions options_;
    rodaq::TaskConfig config_;
    std::unique_ptr<rodaq::TaskService> service_;
};

TEST_F(TaskServiceTest, FormalCreateAssignsStableIdsAndQueriesOnlyPublishedSnapshotsAcrossRestart) {
    open();
    ASSERT_TRUE(service_);
    auto created = service_->create_task(config_, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    auto config = service_->get_active_config(id);
    ASSERT_TRUE(config.ok());
    EXPECT_EQ(config.value().pipelines[0].pipeline_id, 1);
    EXPECT_EQ(config.value().pipelines[0].streams[0].stream_id, 1);
    auto snapshot = service_->get_task(id);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.value().desired_state, rodaq::DesiredState::Stopped);
    EXPECT_EQ(snapshot.value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(snapshot.value().control_generation, 1);
    EXPECT_FALSE(snapshot.value().current_operation);
    auto retry = service_->create_task(config_, {"create"});
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value(), id);
    auto changed = config_;
    changed.name = "changed";
    EXPECT_EQ(service_->create_task(changed, {"create"}).status().code(), StatusCode::Conflict);
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_EQ(task_directories(), 1);
    restart();
    ASSERT_TRUE(service_);
    EXPECT_EQ(service_->create_task(config_, {"create"}).value(), id);
    EXPECT_EQ(service_->get_active_config(id).value().pipelines[0].streams[0].stream_id, 1);
    EXPECT_TRUE(service_->list_operations(id, {}).value().items.empty());
}

TEST_F(
    TaskServiceTest, ExactInstalledAllowedPluginRoleAndDeploymentReferencesAreCheckedBeforeClaim) {
    open();
    ASSERT_TRUE(service_);
    auto config = config_;
    config.pipelines[0].acquisition.plugin_version = "2.0.0";
    EXPECT_EQ(
        service_->create_task(config, {"unknown-version"}).status().code(),
        StatusCode::PermissionDenied);
    config = config_;
    std::swap(config.pipelines[0].acquisition, config.pipelines[0].storage);
    EXPECT_EQ(
        service_->create_task(config, {"wrong-role"}).status().code(),
        StatusCode::PermissionDenied);
    config = config_;
    config.pipelines[0].output_root = "unmanaged";
    EXPECT_EQ(
        service_->create_task(config, {"unknown-volume"}).status().code(),
        StatusCode::PermissionDenied);
    EXPECT_EQ(service_->create_task(config_, {" "}).status().code(), StatusCode::InvalidArgument);
    EXPECT_TRUE(service_->list_tasks({}).value().items.empty());
    EXPECT_EQ(task_directories(), 0);
}

TEST_F(TaskServiceTest, NativeExecutionRequiresIndependentCapacityForEveryAdmittedTask) {
    options_.max_tasks = 2;
    auto single = rodaq::ExecutionLane::create(1, 8).value();
    auto preparation = rodaq::ExecutionLane::create(2, 8).value();
    auto cleanup = rodaq::ExecutionLane::create(2, 8).value();
    auto execution = std::make_shared<task::RunExecutionOptions>();
    execution->resolve = [](const auto&, const auto&, auto&) -> rodaq::Result<task::RunInputs> {
        return rodaq::Status(StatusCode::Unavailable, {});
    };
    execution->commit = [](const auto&) -> rodaq::Result<void> { return {}; };
    execution->reserve_segment = []() -> rodaq::Result<std::uint64_t> { return 1; };
    execution->preparation_lane = single;
    execution->cleanup_lane = cleanup;
    options_.execution = execution;
    EXPECT_EQ(rodaq::TaskService::open(options_).status().code(), StatusCode::InvalidArgument);
    execution->preparation_lane = preparation;
    execution->cleanup_lane = single;
    EXPECT_EQ(rodaq::TaskService::open(options_).status().code(), StatusCode::InvalidArgument);
    execution->cleanup_lane = cleanup;
    auto admitted = rodaq::TaskService::open(options_);
    EXPECT_TRUE(admitted.ok());
    EXPECT_TRUE(single->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_TRUE(preparation->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_TRUE(cleanup->shutdown(std::chrono::steady_clock::now() + 2s).ok());
}

TEST_F(TaskServiceTest, ConcurrentSameKeyCreateReturnsOneTaskAndBoundedCapacityStillAllowsRetry) {
    options_.max_tasks = 1;
    open();
    ASSERT_TRUE(service_);
    auto first =
        std::async(std::launch::async, [&] { return service_->create_task(config_, {"same"}); });
    auto second = service_->create_task(config_, {"same"});
    auto one = first.get();
    ASSERT_TRUE(one.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(one.value(), second.value());
    EXPECT_EQ(
        service_->create_task(config_, {"new-key"}).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(service_->create_task(config_, {"same"}).value(), one.value());
    EXPECT_EQ(task_directories(), 1);
}

TEST_F(TaskServiceTest, IncompleteCreateUsesRecoveryCapacityWithoutConsumingAnActiveTaskSlot) {
    options_.max_tasks = 1;
    options_.create_recovery_slots = 1;
    std::atomic<bool> fail{true};
    options_.materialization_faults = [&](auto step) {
        return fail && step == task::MaterializationStep::Allocated ? EIO : 0;
    };
    open();
    ASSERT_TRUE(service_);
    EXPECT_FALSE(service_->create_task(config_, {"interrupted"}).ok());
    EXPECT_TRUE(service_->list_tasks({}).value().items.empty());
    fail = false;
    const auto created = service_->create_task(config_, {"healthy"});
    ASSERT_TRUE(created.ok()) << static_cast<int>(created.status().code());
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_EQ(service_->create_task(config_, {"healthy"}).value(), created.value());
    EXPECT_EQ(
        service_->create_task(config_, {"interrupted"}).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(task_directories(), 1);
    restart();
    ASSERT_TRUE(service_);
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_EQ(service_->create_task(config_, {"healthy"}).value(), created.value());
}

TEST_F(TaskServiceTest, CreateRecoveryReceiptsHaveAFixedBoundAndSameKeyCanFinishAtThatBound) {
    options_.max_tasks = 1;
    options_.create_recovery_slots = 1;
    std::atomic<bool> fail{true};
    options_.materialization_faults = [&](auto step) {
        return fail && step == task::MaterializationStep::Allocated ? EIO : 0;
    };
    open();
    ASSERT_TRUE(service_);
    EXPECT_FALSE(service_->create_task(config_, {"first"}).ok());
    EXPECT_FALSE(service_->create_task(config_, {"second"}).ok());
    EXPECT_EQ(
        service_->create_task(config_, {"overflow"}).status().code(),
        StatusCode::ResourceExhausted);
    EXPECT_EQ(task_directories(), 0);
    fail = false;
    const auto completed = service_->create_task(config_, {"first"});
    ASSERT_TRUE(completed.ok());
    EXPECT_EQ(service_->create_task(config_, {"first"}).value(), completed.value());
    EXPECT_EQ(
        service_->create_task(config_, {"second"}).status().code(), StatusCode::ResourceExhausted);
    auto different = config_;
    different.name = "Conflicting input at capacity";
    EXPECT_EQ(service_->create_task(different, {"first"}).status().code(), StatusCode::Conflict);
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_EQ(task_directories(), 1);
}

TEST_F(TaskServiceTest, UncertainCompletedAuthorityOwnsCapacityBeforeCatalogVisibility) {
    options_.max_tasks = 1;
    options_.create_recovery_slots = 1;
    std::atomic<bool> fail{false};
    options_.materialization_faults = [&](auto step) {
        if (step == task::MaterializationStep::Control) {
            fail = true;
        }
        return 0;
    };
    options_.file_faults = [&](auto domain, auto step) {
        return fail && domain == task::ServiceFileDomain::CreateReceipts &&
                       step == rodaq::persistence::FileStep::SyncDirectory
                   ? EIO
                   : 0;
    };
    open();
    ASSERT_TRUE(service_);
    EXPECT_FALSE(service_->create_task(config_, {"original"}).ok());
    EXPECT_TRUE(service_->list_tasks({}).value().items.empty());
    options_.materialization_faults = {};
    fail = false;
    EXPECT_EQ(
        service_->create_task(config_, {"other"}).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(task_directories(), 1);
    const auto original = service_->create_task(config_, {"original"});
    ASSERT_TRUE(original.ok());
    restart();
    ASSERT_TRUE(service_);
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_EQ(service_->create_task(config_, {"original"}).value(), original.value());
}

TEST_F(TaskServiceTest, CompletedCreateRetryPrecedesChangedDeploymentAdmission) {
    open();
    ASSERT_TRUE(service_);
    auto created = service_->create_task(config_, {"original"});
    ASSERT_TRUE(created.ok());
    options_.allowed_plugins[0].role = task::AdmissionRole::Storage;
    restart();
    ASSERT_TRUE(service_);
    EXPECT_EQ(service_->create_task(config_, {"original"}).value(), created.value());
    EXPECT_EQ(
        service_->create_task(config_, {"new"}).status().code(), StatusCode::PermissionDenied);
    EXPECT_EQ(task_directories(), 1);
}

TEST_F(TaskServiceTest, KeysetTaskPagesRemainScopedAndDoNotScanPersistentState) {
    open();
    ASSERT_TRUE(service_);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(service_->create_task(config_, {"key-" + std::to_string(i)}).ok());
    }
    auto first = service_->list_tasks({{}, 1});
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(first.value().has_more);
    auto second = service_->list_tasks({first.value().next_cursor, 1});
    ASSERT_TRUE(second.ok());
    EXPECT_TRUE(first.value().items[0].task.task_id < second.value().items[0].task.task_id);
    EXPECT_EQ(
        service_->list_tasks({"operations:v1:bad", 1}).status().code(),
        StatusCode::InvalidArgument);
    // Rename authority directories after admission: queries use immutable memory
    // projections, rather than trying to prove runtime status through scans.
    fs::rename(root_ / "state/control", root_ / "state/control-away");
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 3);
    EXPECT_TRUE(service_->get_task(first.value().items[0].task.task_id).ok());
    fs::rename(root_ / "state/control-away", root_ / "state/control");
}

TEST_F(
    TaskServiceTest,
    UncertainCompletedReceiptStaysInvisibleUntilActualReconciliationThenRetriesSameTask) {
    std::atomic<bool> fail{false};
    options_.materialization_faults = [&](auto step) {
        if (step == task::MaterializationStep::Control) {
            fail = true;
        }
        return 0;
    };
    options_.file_faults = [&](auto domain, auto step) {
        return fail && domain == task::ServiceFileDomain::CreateReceipts &&
                       step == rodaq::persistence::FileStep::SyncDirectory
                   ? EIO
                   : 0;
    };
    open();
    ASSERT_TRUE(service_);
    EXPECT_FALSE(service_->create_task(config_, {"create"}).ok());
    EXPECT_TRUE(service_->list_tasks({}).value().items.empty());
    EXPECT_EQ(task_directories(), 1);
    fail = false;
    auto retried = service_->create_task(config_, {"create"});
    ASSERT_TRUE(retried.ok());
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_EQ(task_directories(), 1);
}

TEST_F(TaskServiceTest, ObserverTimeoutAndFacadeReleaseRetainActualWorkUntilLaneShutdownCompletes) {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    options_.request_wait = 5ms;
    options_.materialization_faults = [&](auto step) {
        if (step == task::MaterializationStep::Control) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        return 0;
    };
    open();
    ASSERT_TRUE(service_);
    auto result = service_->create_task(config_, {"create"});
    EXPECT_EQ(result.status().code(), StatusCode::DeadlineExceeded);
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return entered; }));
    }
    EXPECT_TRUE(service_->list_tasks({}).value().items.empty());
    service_.reset();
    EXPECT_EQ(lane_->snapshot().running, 1);
    auto duplicate = rodaq::TaskService::open(options_);
    EXPECT_EQ(duplicate.status().code(), StatusCode::Conflict);
    EXPECT_EQ(
        lane_->shutdown(std::chrono::steady_clock::now() + 1ms).status().code(),
        StatusCode::DeadlineExceeded);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_all();
    ASSERT_TRUE(lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    auto next_lane = rodaq::ExecutionLane::create(1, 8);
    ASSERT_TRUE(next_lane.ok());
    lane_ = std::move(next_lane.value());
    options_.create_lane = lane_;
    options_.materialization_faults = {};
    open();
    ASSERT_TRUE(service_);
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_TRUE(service_->create_task(config_, {"create"}).ok());
}

TEST_F(
    TaskServiceTest, ConfigureUsesFormalIdentityAndPreconditionAndPublishesTerminalThroughControl) {
    open();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(config_, {"create"}).value();
    auto changed = service_->get_active_config(id).value();
    changed.name = "configured";
    changed.pipelines[0].streams.push_back({0, "second", {}, rodaq::ReadinessPolicy::EntryReady});
    changed.pipelines[0].acquisition.json = R"({"streams":["sample","second"]})";
    auto operation = service_->configure(id, changed, {"configure"}, {true, 1});
    ASSERT_TRUE(operation.ok());
    auto retry = service_->configure(id, changed, {"configure"}, {true, 1});
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value(), operation.value());
    auto wrong = changed;
    wrong.name = "different";
    EXPECT_EQ(
        service_->configure(id, wrong, {"configure"}, {true, 1}).status().code(),
        StatusCode::Conflict);
    EXPECT_EQ(
        service_->configure(id, changed, {"fresh-stale"}, {true, 1}).status().code(),
        StatusCode::Conflict);
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    auto selected = service_->get_active_config(id).value();
    EXPECT_EQ(selected.name, "configured");
    EXPECT_EQ(selected.pipelines[0].streams[0].stream_id, 1);
    EXPECT_EQ(selected.pipelines[0].streams[1].stream_id, 2);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    EXPECT_EQ(
        service_->get_operation(id, operation.value()).value().state,
        rodaq::OperationState::Succeeded);
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 1);
    restart();
    ASSERT_TRUE(service_);
    EXPECT_EQ(
        service_->configure(id, changed, {"configure"}, {true, 1}).value(), operation.value());
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    EXPECT_EQ(service_->get_active_config(id).value().pipelines[0].streams[1].stream_id, 2);
}

TEST_F(TaskServiceTest, RetainedHistoryBoundariesAndConfigureRecoverySurviveTruncationAndRestart) {
    options_.store.history = {4, 1, 1ns};
    open();
    ASSERT_TRUE(service_);
    const auto created = service_->create_task(config_, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    const auto wait_terminal = [&](rodaq::OperationId operation) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
            const auto actual = service_->get_operation(id, operation);
            if (actual.ok() && actual.value().state == rodaq::OperationState::Succeeded) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    std::vector<rodaq::OperationId> operations;
    std::string old_cursor;
    auto changed = service_->get_active_config(id).value();
    for (unsigned i = 0; i < 4; ++i) {
        changed.name = "configured-" + std::to_string(i);
        const auto accepted =
            service_->configure(id, changed, {"configure-" + std::to_string(i)}, {true, i + 1});
        ASSERT_TRUE(accepted.ok());
        ASSERT_TRUE(wait_terminal(accepted.value()));
        operations.push_back(accepted.value());
        if (i == 1) {
            const auto page = service_->list_operations(id, {{}, 1});
            ASSERT_TRUE(page.ok());
            ASSERT_TRUE(page.value().has_more);
            old_cursor = page.value().next_cursor;
        }
    }
    const auto retained = service_->list_operations(id, {});
    ASSERT_TRUE(retained.ok());
    ASSERT_TRUE(retained.value().history_truncated);
    ASSERT_EQ(retained.value().items.size(), 2);
    EXPECT_EQ(retained.value().oldest_retained_control_generation, 4);
    EXPECT_EQ(retained.value().oldest_retained_at, retained.value().items.front().accepted_at);
    EXPECT_EQ(
        service_->get_operation(id, operations.front()).status().code(), StatusCode::NotFound);
    const auto stale_cursor = service_->list_operations(id, {old_cursor, 8});
    ASSERT_TRUE(stale_cursor.ok());
    EXPECT_TRUE(stale_cursor.value().history_truncated);
    EXPECT_EQ(stale_cursor.value().items.size(), 2);
    restart();
    ASSERT_TRUE(service_);
    const auto restored = service_->list_operations(id, {});
    ASSERT_TRUE(restored.ok());
    EXPECT_TRUE(restored.value().history_truncated);
    EXPECT_EQ(
        restored.value().oldest_retained_control_generation,
        retained.value().oldest_retained_control_generation);
    EXPECT_EQ(restored.value().oldest_retained_at, retained.value().oldest_retained_at);
    EXPECT_EQ(
        service_->configure(id, changed, {"configure-3"}, {true, 4}).value(), operations.back());
    changed.name = "configured-after-restart";
    const auto next = service_->configure(id, changed, {"next"}, {true, 5});
    ASSERT_TRUE(next.ok()) << static_cast<int>(next.status().code());
    ASSERT_TRUE(wait_terminal(next.value()));
    EXPECT_EQ(service_->get_task(id).value().control_generation, 6);
    EXPECT_LE(service_->list_operations(id, {}).value().items.size(), 3);
}

TEST_F(TaskServiceTest, TruncatedStoppedHistoryWithoutConfigurationDoesNotProveQuiescence) {
    open();
    ASSERT_TRUE(service_);
    const auto created = service_->create_task(config_, {"create"});
    ASSERT_TRUE(created.ok());
    const auto id = created.value();
    service_.reset();
    ASSERT_TRUE(lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    {
        // Model a valid stopped authority whose older execution history expired.
        // Stop's desired state alone carries no physical cleanup proof.
        auto store = task::TaskStore::open(
            options_.root / "tasks" / rodaq::to_string(id), options_.root / "control", id,
            task::TaskStoreMode::Existing, options_.store);
        ASSERT_TRUE(store.ok());
        auto loaded = store.value()->load_control();
        ASSERT_TRUE(loaded.ok());
        auto record = std::move(loaded.value());
        const auto previous_revision = record.record_revision++;
        ++record.control_generation;
        task::StoredOperation stop;
        stop.request = {"stopped"};
        auto digest = task::lifecycle_request_digest(id, rodaq::OperationKind::Stop, {true, 1});
        ASSERT_TRUE(digest.ok());
        stop.request_digest = std::move(digest.value());
        stop.snapshot.operation_id.value[15] = 1;
        stop.snapshot.kind = rodaq::OperationKind::Stop;
        stop.snapshot.control_generation = record.control_generation;
        stop.snapshot.state = rodaq::OperationState::Accepted;
        stop.snapshot.accepted_at = rodaq::WallTime{1s};
        record.current_operation_id = stop.snapshot.operation_id;
        record.operation_history.entries.push_back(std::move(stop));
        record.operation_history.history_truncated = true;
        ASSERT_TRUE(store.value()->publish_control(record, previous_revision).ok());
        const auto accepted_revision = record.record_revision++;
        auto& terminal = record.operation_history.entries.back().snapshot;
        terminal.state = rodaq::OperationState::Succeeded;
        terminal.terminal_at = rodaq::WallTime{2s};
        ASSERT_TRUE(store.value()->publish_control(record, accepted_revision).ok());
    }
    restart();
    ASSERT_TRUE(service_);
    auto changed = service_->get_active_config(id).value();
    changed.name = "must-remain-guarded";
    EXPECT_EQ(
        service_->configure(id, changed, {"configure"}, {true, 2}).status().code(),
        StatusCode::InvalidState);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 1);
}

TEST_F(
    TaskServiceTest,
    ConfigureRetriesMergeWhileActualPersistenceIsBlockedAndQueriesRemainNonblocking) {
    std::atomic<bool> armed{false};
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    options_.file_faults = [&](auto domain, auto step) {
        if (domain == task::ServiceFileDomain::Task &&
            step == rodaq::persistence::FileStep::SyncFile && armed.exchange(false)) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        return 0;
    };
    open();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(config_, {"create"}).value();
    auto changed = service_->get_active_config(id).value();
    changed.name = "changed";
    armed = true;
    auto first = std::async(std::launch::async, [&] {
        return service_->configure(id, changed, {"configure"}, {true, 1});
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return entered; }));
    }
    auto retry = std::async(std::launch::async, [&] {
        return service_->configure(id, changed, {"configure"}, {true, 1});
    });
    EXPECT_EQ(first.wait_for(1ms), std::future_status::timeout);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 1);
    EXPECT_TRUE(service_->get_active_config(id).value().name.empty());
    EXPECT_EQ(
        service_->configure(id, changed, {"wrong-generation"}, {true, 99}).status().code(),
        StatusCode::Conflict);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_all();
    auto accepted = first.get();
    auto merged = retry.get();
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(merged.ok());
    EXPECT_EQ(accepted.value(), merged.value());
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 1);
}

TEST_F(TaskServiceTest, FailedConfigureRestoresActualActiveBindingsWithoutReusingDurableHighWater) {
    std::atomic<bool> arm{false};
    std::atomic<bool> fail{false};
    options_.file_faults = [&](auto domain, auto step) {
        if (domain == task::ServiceFileDomain::StreamIdentity &&
            step == rodaq::persistence::FileStep::SyncDirectory && arm.exchange(false)) {
            fail = true;
        }
        return domain == task::ServiceFileDomain::Task &&
                       step == rodaq::persistence::FileStep::Publish && fail
                   ? EIO
                   : 0;
    };
    open();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(config_, {"create"}).value();
    auto original = service_->get_active_config(id).value();
    auto changed = original;
    changed.pipelines[0].streams.push_back({0, "failed", {}, rodaq::ReadinessPolicy::EntryReady});
    arm = true;
    EXPECT_FALSE(service_->configure(id, changed, {"failed"}, {true, 1}).ok());
    EXPECT_EQ(service_->get_task(id).value().control_generation, 1);
    EXPECT_EQ(service_->get_active_config(id).value().pipelines[0].streams.size(), 1);
    fail = false;
    auto replacement = original;
    replacement.pipelines[0].streams.push_back(
        {0, "replacement", {}, rodaq::ReadinessPolicy::EntryReady});
    auto accepted = service_->configure(id, replacement, {"replacement"}, {true, 1});
    ASSERT_TRUE(accepted.ok());
    EXPECT_EQ(service_->get_active_config(id).value().pipelines[0].streams[1].stream_id, 3);
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(
        service_->get_operation(id, accepted.value()).value().state,
        rodaq::OperationState::Succeeded);
}

TEST_F(
    TaskServiceTest,
    UncertainConfigureRetriesReconcileOneOperationWithoutReallocationOrEarlyAccepted) {
    std::atomic<bool> arm{false};
    std::atomic<bool> fail{false};
    options_.file_faults = [&](auto domain, auto step) {
        if (domain == task::ServiceFileDomain::StreamIdentity &&
            step == rodaq::persistence::FileStep::SyncDirectory && arm.exchange(false)) {
            fail = true;
        }
        return domain == task::ServiceFileDomain::Task &&
                       step == rodaq::persistence::FileStep::SyncDirectory && fail
                   ? EIO
                   : 0;
    };
    open();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(config_, {"create"}).value();
    auto changed = service_->get_active_config(id).value();
    changed.pipelines[0].streams.push_back({0, "second", {}, rodaq::ReadinessPolicy::EntryReady});
    arm = true;
    EXPECT_FALSE(service_->configure(id, changed, {"uncertain"}, {true, 1}).ok());
    EXPECT_EQ(service_->get_task(id).value().control_generation, 1);
    EXPECT_EQ(
        service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Protected);
    fail = false;
    auto accepted = service_->configure(id, changed, {"uncertain"}, {true, 1});
    ASSERT_TRUE(accepted.ok());
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    EXPECT_EQ(service_->get_active_config(id).value().pipelines[0].streams[1].stream_id, 2);
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 1);
    EXPECT_EQ(
        service_->get_operation(id, accepted.value()).value().state,
        rodaq::OperationState::Succeeded);
}

TEST_F(
    TaskServiceTest,
    ConfigureObserverTimeoutDoesNotReleaseActualPersistenceOrAllocateASecondOperation) {
    std::atomic<bool> arm{false};
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    options_.request_wait = 1s;
    options_.file_faults = [&](auto domain, auto step) {
        if (domain == task::ServiceFileDomain::Task &&
            step == rodaq::persistence::FileStep::SyncFile && arm.exchange(false)) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        return 0;
    };
    open();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(config_, {"create"}).value();
    auto changed = service_->get_active_config(id).value();
    changed.name = "changed";
    arm = true;
    auto observed = service_->configure(id, changed, {"configure"}, {true, 1});
    EXPECT_EQ(observed.status().code(), StatusCode::DeadlineExceeded);
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return entered; }));
    }
    EXPECT_EQ(control_lane_->snapshot().running, 1);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 1);
    auto retry = std::async(std::launch::async, [&] {
        return service_->configure(id, changed, {"configure"}, {true, 1});
    });
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_all();
    auto accepted = retry.get();
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    EXPECT_EQ(service_->list_operations(id, {}).value().items.size(), 1);
    EXPECT_EQ(
        service_->get_operation(id, accepted.value()).value().state,
        rodaq::OperationState::Succeeded);
}

TEST_F(
    TaskServiceTest,
    FailedTerminalPublicationKeepsAcceptedIdentityAndProtectedUntilStartupRecovery) {
    std::atomic<bool> arm{false};
    std::atomic<bool> identity_ready{false};
    std::atomic<int> publications{0};
    options_.file_faults = [&](auto domain, auto step) {
        if (!arm) {
            return 0;
        }
        if (domain == task::ServiceFileDomain::StreamIdentity &&
            step == rodaq::persistence::FileStep::SyncDirectory) {
            identity_ready = true;
        }
        if (identity_ready && domain == task::ServiceFileDomain::Task &&
            step == rodaq::persistence::FileStep::Publish && ++publications == 2) {
            return EIO;
        }
        return 0;
    };
    open();
    ASSERT_TRUE(service_);
    auto id = service_->create_task(config_, {"create"}).value();
    auto changed = service_->get_active_config(id).value();
    changed.name = "configured";
    arm = true;
    auto accepted = service_->configure(id, changed, {"configure"}, {true, 1});
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(
        service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Protected);
    EXPECT_EQ(
        service_->get_operation(id, accepted.value()).value().state,
        rodaq::OperationState::Accepted);
    EXPECT_EQ(service_->get_active_config(id).value().name, "configured");
    restart();
    ASSERT_TRUE(service_);
    EXPECT_EQ(service_->get_task(id).value().control_generation, 2);
    EXPECT_EQ(
        service_->get_operation(id, accepted.value()).value().state,
        rodaq::OperationState::Succeeded);
    EXPECT_EQ(service_->get_task(id).value().execution_condition, rodaq::ExecutionCondition::Idle);
    EXPECT_EQ(service_->configure(id, changed, {"configure"}, {true, 1}).value(), accepted.value());
}

class ServiceCrashTest : public TaskServiceTest,
                         public testing::WithParamInterface<task::MaterializationStep> {};

TEST_P(ServiceCrashTest, LiveProcessCrashAtEveryMaterializationBoundaryRetriesOneTaskIdentity) {
    ASSERT_TRUE(crash_at(static_cast<int>(GetParam())));
    std::optional<rodaq::TaskId> original_id;
    for (const auto& file : fs::directory_iterator(root_ / "state/create")) {
        if (file.path().extension() == ".json") {
            std::ifstream input(file.path());
            auto document = rodaq::persistence::Json::parse(input);
            original_id =
                rodaq::parse_task_id(document.at("assigned_task_id").get<std::string>()).value();
        }
    }
    ASSERT_TRUE(original_id);
    open();
    ASSERT_TRUE(service_);
    EXPECT_EQ(
        service_->list_tasks({}).value().items.size(),
        GetParam() == task::MaterializationStep::Completed ? 1U : 0U);
    auto retried = service_->create_task(config_, {"crash-create"});
    ASSERT_TRUE(retried.ok());
    EXPECT_EQ(retried.value(), original_id.value());
    EXPECT_EQ(service_->list_tasks({}).value().items.size(), 1);
    EXPECT_EQ(task_directories(), 1);
    EXPECT_EQ(service_->create_task(config_, {"crash-create"}).value(), retried.value());
}

class ConfigureCrashTest : public TaskServiceTest, public testing::WithParamInterface<int> {};

TEST_P(ConfigureCrashTest, CrashSelectsActualControlAndPreservesBurntIdsOrAcceptedOperation) {
    ASSERT_TRUE(crash_at(GetParam(), "configure"));
    std::optional<task::TaskControlRecord> before;
    for (const auto& file : fs::directory_iterator(root_ / "state/control")) {
        if (file.path().extension() == ".json") {
            auto bytes = rodaq::persistence::read_file(
                file.path().parent_path(), file.path().filename().string(),
                task::control_record_max_bytes(options_.store.history));
            ASSERT_TRUE(bytes.ok());
            auto decoded = task::decode_control_record(bytes.value(), options_.store.history);
            ASSERT_TRUE(decoded.ok());
            before = std::move(decoded.value());
        }
    }
    ASSERT_TRUE(before);
    EXPECT_EQ(before->control_generation, GetParam() == 0 ? 1U : 2U);
    open();
    ASSERT_TRUE(service_);
    auto request = service_->get_active_config(before->task_id).value();
    request.name = "crash-configured";
    request.pipelines[0].streams.resize(1);
    request.pipelines[0].streams.push_back({0, "second", {}, rodaq::ReadinessPolicy::EntryReady});
    request.pipelines[0].acquisition.json = R"({"streams":["sample","second"]})";
    auto retried = service_->configure(before->task_id, request, {"crash-configure"}, {true, 1});
    ASSERT_TRUE(retried.ok());
    if (GetParam() == 1) {
        ASSERT_TRUE(before->current_operation_id);
        EXPECT_EQ(retried.value(), *before->current_operation_id);
    }
    ASSERT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
    EXPECT_EQ(service_->get_task(before->task_id).value().control_generation, 2);
    EXPECT_EQ(
        service_->get_active_config(before->task_id).value().pipelines[0].streams[1].stream_id,
        GetParam() == 0 ? 3U : 2U);
    EXPECT_EQ(
        service_->get_operation(before->task_id, retried.value()).value().state,
        rodaq::OperationState::Succeeded);
    EXPECT_EQ(service_->list_operations(before->task_id, {}).value().items.size(), 1);
}

INSTANTIATE_TEST_SUITE_P(ConfigureCrashBoundaries, ConfigureCrashTest, testing::Values(0, 1));

INSTANTIATE_TEST_SUITE_P(
    CreateCrashBoundaries, ServiceCrashTest,
    testing::Values(
        task::MaterializationStep::Allocated, task::MaterializationStep::Materializing,
        task::MaterializationStep::Config, task::MaterializationStep::Identity,
        task::MaterializationStep::Control, task::MaterializationStep::Completed));

}  // namespace
