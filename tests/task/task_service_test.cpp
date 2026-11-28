#include "service_config.hpp"
#include "task/identity.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace task = rodaq::task;
using rodaq::ControlPrecondition;
using rodaq::DesiredState;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::RequestIdentity;
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
        service_.reset();
        if (lane_) {
            EXPECT_TRUE(lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
        }
        if (control_lane_) {
            EXPECT_TRUE(control_lane_->shutdown(std::chrono::steady_clock::now() + 2s).ok());
        }
        if (manager_) {
            EXPECT_TRUE(manager_->shutdown(rodaq::plugin::Deadline::after(2s).value()).ok());
        }
        manager_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void open_service() {
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
        open_service();
    }

    fs::path root_;
    std::shared_ptr<rodaq::plugin::PluginManager> manager_;
    std::shared_ptr<rodaq::ExecutionLane> lane_;
    std::shared_ptr<rodaq::ExecutionLane> control_lane_;
    task::ServiceOptions options_;
    rodaq::TaskConfig config_;
    std::unique_ptr<rodaq::TaskService> service_;
};

TEST_F(TaskServiceTest, CreateAssignsStableIdsAndSurvivesRestartAsStoppedWithEmptyOperations) {
    open_service();
    auto created = service_->create_task(config_, RequestIdentity{"create-1"});
    ASSERT_TRUE(created.ok()) << created.status().message();
    const auto task_id = created.value();

    // Same request_key + same config returns same task_id (in-process idempotency).
    auto dup = service_->create_task(config_, RequestIdentity{"create-1"});
    ASSERT_TRUE(dup.ok());
    EXPECT_EQ(dup.value(), task_id);

    // Same request_key + different config -> Conflict.
    auto changed = config_;
    changed.name = "changed";
    EXPECT_EQ(
        service_->create_task(changed, RequestIdentity{"create-1"}).status().code(),
        StatusCode::Conflict);

    auto detail = service_->get_task(task_id);
    ASSERT_TRUE(detail.ok());
    EXPECT_EQ(detail.value().desired_state, DesiredState::Stopped);
    EXPECT_EQ(detail.value().control_generation, 1ULL);
    auto active_cfg = service_->get_active_config(task_id);
    ASSERT_TRUE(active_cfg.ok());
    EXPECT_EQ(active_cfg.value().pipelines[0].pipeline_id, 1ULL);
    EXPECT_EQ(active_cfg.value().pipelines[0].streams[0].stream_id, 1ULL);

    // Configure updates active_config_version and advances control_generation.
    auto cfg_update = active_cfg.value();
    cfg_update.name = "renamed";
    auto op = service_->configure(
        task_id, cfg_update, RequestIdentity{"cfg-1"}, ControlPrecondition{true, 1});
    ASSERT_TRUE(op.ok()) << op.status().message();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto snap = service_->get_operation(task_id, op.value());
        if (snap.ok() && snap.value().state == OperationState::Succeeded) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    auto op_snap = service_->get_operation(task_id, op.value());
    ASSERT_TRUE(op_snap.ok());
    EXPECT_EQ(op_snap.value().state, OperationState::Succeeded);

    // Restart: Task survives as Stopped with updated config; in-memory Operation returns NotFound!
    restart();
    auto after_restart = service_->get_task(task_id);
    ASSERT_TRUE(after_restart.ok());
    EXPECT_EQ(after_restart.value().desired_state, DesiredState::Stopped);
    EXPECT_EQ(after_restart.value().control_generation, 1ULL);
    auto after_cfg = service_->get_active_config(task_id);
    ASSERT_TRUE(after_cfg.ok());
    EXPECT_EQ(after_cfg.value().name, "renamed");
    EXPECT_EQ(service_->get_operation(task_id, op.value()).status().code(), StatusCode::NotFound);
    auto ops_page = service_->list_operations(task_id, {"", 10});
    ASSERT_TRUE(ops_page.ok());
    EXPECT_TRUE(ops_page.value().items.empty());
}

TEST_F(TaskServiceTest, MaxTasksLimitAndAdmissionValidationRejectInvalidConfigs) {
    options_.max_tasks = 1;
    open_service();

    auto bad_plugin = config_;
    bad_plugin.pipelines[0].acquisition.plugin_id = "com.unknown.acq";
    EXPECT_EQ(
        service_->create_task(bad_plugin, RequestIdentity{"bad-1"}).status().code(),
        StatusCode::PermissionDenied);

    auto first = service_->create_task(config_, RequestIdentity{"ok-1"});
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(
        service_->create_task(config_, RequestIdentity{"ok-2"}).status().code(),
        StatusCode::ResourceExhausted);
}

TEST_F(TaskServiceTest, ConcurrentCreateRequestsRespectSlotLimits) {
    options_.max_tasks = 2;
    open_service();
    std::vector<std::future<rodaq::Result<rodaq::TaskId>>> calls;
    for (int i = 0; i < 6; ++i) {
        calls.push_back(std::async(std::launch::async, [&, i] {
            return service_->create_task(config_, RequestIdentity{"conc-" + std::to_string(i)});
        }));
    }
    int succeeded = 0;
    for (auto& fut : calls) {
        if (fut.get().ok()) {
            ++succeeded;
        }
    }
    EXPECT_EQ(succeeded, 2);
}

}  // namespace
