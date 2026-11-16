#include "task/task_controller.hpp"
#include "task/task_store.hpp"
#include "pipeline/admission_gate.hpp"
#include "persistence/json.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {
namespace task = rodaq::task;
namespace persistence = rodaq::persistence;
using rodaq::DesiredState;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::Status;
using rodaq::StatusCode;
using namespace std::chrono_literals;

class TaskControllerTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 48> pattern{};
        const std::string prefix = "/tmp/rodaq-task-controller-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        ASSERT_TRUE(std::filesystem::create_directories(root_ / "task/configs"));
        ASSERT_TRUE(std::filesystem::create_directory(root_ / "control"));
        id_ = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        std::ifstream input(RODAQ_TASK_CONFIG_FIXTURE);
        auto config = rodaq::parse_task_config(persistence::Json::parse(input).dump());
        ASSERT_TRUE(config.ok());
        auto opened = task::TaskStore::open(
            root_ / "task", root_ / "control", id_, task::TaskStoreMode::Materializing, {},
            [this](auto step) { return fault(step); });
        ASSERT_TRUE(opened.ok());
        store_ = std::move(opened.value());
        auto created_config = store_->create_config(config.value());
        ASSERT_TRUE(created_config.ok());
        initial_.task_id = id_;
        initial_.active_config_version = created_config.value().version;
        ASSERT_TRUE(store_->publish_control(initial_, 0).ok());
        auto owner = task::TaskController::create(initial_);
        ASSERT_TRUE(owner.ok());
        controller_ = std::move(owner.value());
    }

    void TearDown() override {
        store_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    int fault(persistence::FileStep step) {
        if (delay_ && step == persistence::FileStep::SyncFile) {
            std::unique_lock<std::mutex> lock(mutex_);
            entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return released_; });
        }
        if (fail_ && step == failed_step_) {
            if (one_shot_) {
                fail_ = false;
            }
            return EIO;
        }
        return 0;
    }

    auto request(
        OperationKind kind, std::string key, rodaq::ControlPrecondition precondition = {}) {
        return controller_->request_lifecycle(
            kind, {std::move(key)}, precondition, rodaq::WallTime{1s}, {true, true});
    }

    auto actual_publish(const std::shared_ptr<const task::ControlWork>& work) {
        return store_->publish_control(*work->candidate, work->expected_revision);
    }

    task::ControlCompletion publish(const std::shared_ptr<const task::ControlWork>& work) {
        EXPECT_TRUE(work);
        if (!work) {
            return {};
        }
        auto actual = task::execute_control_work(*store_, *work);
        auto completed = controller_->persistence_completed(
            actual.work_id, std::move(actual.authority), std::move(actual.publication_failure));
        EXPECT_TRUE(completed.ok());
        return completed.ok() ? std::move(completed.value()) : task::ControlCompletion{};
    }

    const rodaq::OperationSnapshot& current() {
        return controller_->durable_record()->operation_history.entries.back().snapshot;
    }

    bool accept(const std::shared_ptr<const task::ControlWork>& work) {
        const auto completed = publish(work);
        return completed.resolved && completed.resolved->durable;
    }

    std::filesystem::path root_;
    rodaq::TaskId id_;
    task::TaskControlRecord initial_;
    std::unique_ptr<task::TaskStore> store_;
    std::unique_ptr<task::TaskController> controller_;
    std::atomic<bool> fail_{false};
    std::atomic<bool> delay_{false};
    std::atomic<bool> one_shot_{false};
    persistence::FileStep failed_step_{persistence::FileStep::Publish};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{false};
    bool released_{false};
};

TEST_F(
    TaskControllerTest, PendingRetryMergesBeforePreconditionAndOnlyActualDurablePublishIsAccepted) {
    auto start = request(OperationKind::Start, "start", {true, 1});
    ASSERT_TRUE(start.ok());
    EXPECT_FALSE(start.value().durably_accepted);
    ASSERT_TRUE(start.value().work);
    EXPECT_EQ(controller_->durable_record()->control_generation, 1);
    EXPECT_FALSE(controller_->run_is_current(start.value().operation_id, 2));
    auto retry = request(OperationKind::Start, "start", {true, 1});
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value().operation_id, start.value().operation_id);
    EXPECT_FALSE(retry.value().work);
    EXPECT_FALSE(retry.value().durably_accepted);
    EXPECT_EQ(
        request(OperationKind::Stop, "start", {true, 1}).status().code(), StatusCode::Conflict);
    auto accepted = publish(start.value().work);
    ASSERT_TRUE(accepted.resolved);
    EXPECT_TRUE(accepted.resolved->durable);
    EXPECT_TRUE(controller_->run_is_current(start.value().operation_id, 2));
    retry = request(OperationKind::Start, "start", {true, 1});
    ASSERT_TRUE(retry.ok());
    EXPECT_TRUE(retry.value().durably_accepted);
    EXPECT_EQ(retry.value().operation_id, start.value().operation_id);
    EXPECT_EQ(
        request(OperationKind::Pause, "fresh-stale", {true, 1}).status().code(),
        StatusCode::Conflict);
    EXPECT_EQ(controller_->durable_record()->operation_history.entries.size(), 1);
}

TEST_F(
    TaskControllerTest,
    SupersedingStopFencesBeforeBlockedOldPublishActuallyReturnsAndUsesItsDurableResult) {
    auto start = request(OperationKind::Start, "start").value();
    rodaq::pipeline::InstanceId instance;
    instance.value[0] = 1;
    rodaq::pipeline::AdmissionGate gate(instance, 2);
    delay_ = true;
    auto persistence = std::async(std::launch::async, [&] { return actual_publish(start.work); });
    {
        std::unique_lock<std::mutex> lock(mutex_);
        EXPECT_TRUE(condition_.wait_for(lock, 2s, [&] { return entered_; }));
    }
    auto stop = request(OperationKind::Stop, "stop", {true, 1});
    EXPECT_TRUE(stop.ok());
    if (stop.ok()) {
        EXPECT_TRUE(stop.value().fence_required);
        EXPECT_FALSE(stop.value().durably_accepted);
        EXPECT_FALSE(stop.value().work);
        EXPECT_TRUE(gate.establish_control_fence(instance).ok());
        auto retry = request(OperationKind::Stop, "stop", {true, 1});
        EXPECT_TRUE(retry.ok());
        if (retry.ok()) {
            EXPECT_EQ(retry.value().operation_id, stop.value().operation_id);
        }
    }
    EXPECT_EQ(persistence.wait_for(1ms), std::future_status::timeout);
    EXPECT_FALSE(controller_->run_is_current(start.operation_id, 2));
    EXPECT_EQ(controller_->durable_record()->control_generation, 1);
    EXPECT_EQ(
        request(OperationKind::Stop, "other-stop").status().code(), StatusCode::ResourceExhausted);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
    }
    condition_.notify_all();
    ASSERT_TRUE(persistence.get().ok());
    delay_ = false;
    auto ended = controller_->persistence_completed(start.work->work_id, store_->load_control());
    ASSERT_TRUE(ended.ok());
    ASSERT_TRUE(ended.value().resolved);
    EXPECT_TRUE(ended.value().resolved->durable);
    EXPECT_FALSE(controller_->run_is_current(start.operation_id, 2));
    EXPECT_EQ(gate.open_admission(instance, 2), rodaq::pipeline::GateOpenResult::Obsolete);
    auto next = ended.value().next_work;
    ASSERT_TRUE(next);
    EXPECT_EQ(next->expected_revision, 2);
    EXPECT_EQ(next->candidate->control_generation, 3);
    EXPECT_EQ(
        next->candidate->operation_history.entries[0].snapshot.state, OperationState::Superseded);
    auto accepted = publish(next);
    ASSERT_TRUE(accepted.resolved);
    EXPECT_TRUE(accepted.resolved->durable);
    EXPECT_EQ(controller_->durable_record()->desired_state, DesiredState::Stopped);
    EXPECT_EQ(current().operation_id, stop.value().operation_id);
}

TEST_F(
    TaskControllerTest,
    FailedOldPublishCannotBecomeAcceptedAndStopBuildsFromActualPreviousGeneration) {
    auto start = request(OperationKind::Start, "start").value();
    auto stop = request(OperationKind::Stop, "stop").value();
    fail_ = true;
    auto result = actual_publish(start.work);
    ASSERT_FALSE(result.ok());
    fail_ = false;
    auto actual = store_->reconcile();
    ASSERT_TRUE(actual.ok());
    ASSERT_TRUE(actual.value());
    auto completed =
        controller_->persistence_completed(start.work->work_id, *actual.value(), result.status());
    ASSERT_TRUE(completed.ok());
    ASSERT_TRUE(completed.value().resolved);
    EXPECT_FALSE(completed.value().resolved->durable);
    ASSERT_TRUE(completed.value().next_work);
    EXPECT_EQ(completed.value().next_work->candidate->control_generation, 2);
    auto accepted = publish(completed.value().next_work);
    ASSERT_TRUE(accepted.resolved);
    EXPECT_TRUE(accepted.resolved->durable);
    EXPECT_EQ(current().operation_id, stop.operation_id);
    EXPECT_EQ(controller_->durable_record()->operation_history.entries.size(), 1);
}

TEST_F(
    TaskControllerTest, UncertainAuthorityProtectsUntilFreshReconcileWorkAndNeverReopensFencedRun) {
    auto start = request(OperationKind::Start, "start").value();
    auto stop = request(OperationKind::Stop, "stop").value();
    failed_step_ = persistence::FileStep::SyncDirectory;
    fail_ = true;
    ASSERT_FALSE(actual_publish(start.work).ok());
    auto completed = controller_->persistence_completed(
        start.work->work_id, Status(StatusCode::Unavailable, {}));
    ASSERT_TRUE(completed.ok());
    EXPECT_TRUE(completed.value().persistence_protected);
    EXPECT_FALSE(completed.value().next_work);
    EXPECT_FALSE(completed.value().resolved->durable);
    ASSERT_TRUE(completed.value().stop_resolution);
    EXPECT_EQ(completed.value().stop_resolution->operation_id, stop.operation_id);
    EXPECT_EQ(completed.value().stop_resolution->status.code(), StatusCode::Unavailable);
    auto reconcile = controller_->reconciliation_work();
    ASSERT_TRUE(reconcile.ok());
    EXPECT_NE(reconcile.value()->work_id, start.work->work_id);
    EXPECT_EQ(reconcile.value()->kind, task::ControlWorkKind::Reconcile);
    EXPECT_FALSE(
        controller_->persistence_completed(start.work->work_id, initial_).value().recognized);
    EXPECT_FALSE(controller_->reconciliation_work().ok());
    fail_ = false;
    auto actual = store_->reconcile();
    ASSERT_TRUE(actual.ok());
    ASSERT_TRUE(actual.value());
    completed = controller_->persistence_completed(reconcile.value()->work_id, *actual.value());
    ASSERT_TRUE(completed.ok());
    EXPECT_FALSE(completed.value().persistence_protected);
    EXPECT_TRUE(completed.value().resolved->durable);
    EXPECT_FALSE(controller_->run_is_current(start.operation_id, 2));
    ASSERT_TRUE(completed.value().next_work);
    EXPECT_TRUE(accept(completed.value().next_work));
    EXPECT_EQ(current().operation_id, stop.operation_id);
}

TEST_F(
    TaskControllerTest, ReusedUnpublishedGenerationDoesNotAcceptLateWorkOrOldOperationCompletion) {
    auto first = request(OperationKind::Start, "first").value();
    auto failed = controller_->persistence_completed(
        first.work->work_id, initial_, Status(StatusCode::Unavailable, {}));
    ASSERT_TRUE(failed.ok());
    auto second = request(OperationKind::Start, "second").value();
    EXPECT_EQ(
        first.work->candidate->control_generation, second.work->candidate->control_generation);
    EXPECT_FALSE(first.operation_id == second.operation_id);
    EXPECT_NE(first.work->work_id, second.work->work_id);
    EXPECT_FALSE(controller_->persistence_completed(first.work->work_id, *first.work->candidate)
                     .value()
                     .recognized);
    EXPECT_TRUE(accept(second.work));
    EXPECT_FALSE(controller_->run_is_current(first.operation_id, 2));
    EXPECT_TRUE(controller_->run_is_current(second.operation_id, 2));
    auto stale = first.work->candidate->operation_history.entries.back().snapshot;
    stale.state = OperationState::Succeeded;
    stale.terminal_at = rodaq::WallTime{2s};
    EXPECT_EQ(controller_->update_operation(stale).status().code(), StatusCode::Conflict);
}

TEST_F(
    TaskControllerTest,
    PauseResumeStopAndOperationStatusPublishThroughActualVersionedControlStore) {
    auto start = request(OperationKind::Start, "start").value();
    ASSERT_TRUE(accept(start.work));
    auto result = current();
    result.state = OperationState::Succeeded;
    result.terminal_at = rodaq::WallTime{2s};
    auto status = controller_->update_operation(result);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(status.value()->candidate->control_generation, 2);
    EXPECT_EQ(status.value()->candidate->record_revision, 3);
    EXPECT_EQ(current().state, OperationState::Accepted);
    ASSERT_TRUE(accept(status.value()));
    EXPECT_EQ(current().state, OperationState::Succeeded);
    auto pause = request(OperationKind::Pause, "pause", {true, 2}).value();
    EXPECT_TRUE(pause.fence_required);
    EXPECT_FALSE(controller_->run_is_current(start.operation_id, 2));
    ASSERT_TRUE(accept(pause.work));
    EXPECT_EQ(controller_->durable_record()->desired_state, DesiredState::Paused);
    auto resume = request(OperationKind::Resume, "resume", {true, 3}).value();
    ASSERT_TRUE(accept(resume.work));
    EXPECT_TRUE(controller_->run_is_current(resume.operation_id, 4));
    auto stop = request(OperationKind::Stop, "stop", {true, 4}).value();
    EXPECT_TRUE(stop.fence_required);
    ASSERT_TRUE(accept(stop.work));
    EXPECT_EQ(controller_->durable_record()->desired_state, DesiredState::Stopped);
    EXPECT_TRUE(request(OperationKind::Start, "start").value().durably_accepted);
    EXPECT_EQ(store_->load_control().value().record_revision, 6);
}

TEST_F(
    TaskControllerTest, UnknownAuthorityIsProtectedAndActualEndedFrameAllowsFreshReconciliation) {
    auto start = request(OperationKind::Start, "start").value();
    auto stranger = *start.work->candidate;
    stranger.active_config_version = "sha256:" + std::string(64, 'f');
    auto completed = controller_->persistence_completed(start.work->work_id, stranger);
    ASSERT_TRUE(completed.ok());
    EXPECT_TRUE(completed.value().persistence_protected);
    EXPECT_EQ(completed.value().resolved->status.code(), StatusCode::DataLoss);
    EXPECT_FALSE(controller_->run_is_current(start.operation_id, 2));
    EXPECT_EQ(request(OperationKind::Start, "other").status().code(), StatusCode::InvalidState);
    auto reconcile = controller_->reconciliation_work();
    ASSERT_TRUE(reconcile.ok());
    EXPECT_NE(reconcile.value()->work_id, start.work->work_id);
}

TEST_F(TaskControllerTest, EmergencyHistorySlotRemainsForStopAndOperationMutationIsGuarded) {
    task::HistoryPolicy policy;
    policy.max_entries = 5;
    policy.emergency_entries = 2;
    auto owner = task::TaskController::create(initial_, policy);
    ASSERT_TRUE(owner.ok());
    controller_ = std::move(owner.value());
    ASSERT_TRUE(accept(request(OperationKind::Start, "start").value().work));
    ASSERT_TRUE(accept(request(OperationKind::Pause, "pause").value().work));
    ASSERT_TRUE(accept(request(OperationKind::Resume, "resume").value().work));
    ASSERT_TRUE(accept(request(OperationKind::Pause, "pause-again").value().work));
    EXPECT_EQ(
        request(OperationKind::Resume, "no-space").status().code(), StatusCode::ResourceExhausted);
    auto result = current();
    result.state = OperationState::Succeeded;
    result.terminal_at = rodaq::WallTime{2s};
    result.accepted_at += 1ns;
    EXPECT_EQ(controller_->update_operation(result).status().code(), StatusCode::Conflict);
    auto stop = request(OperationKind::Stop, "stop");
    ASSERT_TRUE(stop.ok());
    EXPECT_TRUE(accept(stop.value().work));
    EXPECT_EQ(controller_->durable_record()->operation_history.entries.size(), 5);
}

TEST_F(TaskControllerTest, ProductionWorkMappingReconcilesRealFailureBeforeAndAfterRename) {
    auto start = request(OperationKind::Start, "not-published").value();
    one_shot_ = true;
    fail_ = true;
    auto actual = task::execute_control_work(*store_, *start.work);
    ASSERT_TRUE(actual.authority.ok());
    EXPECT_FALSE(actual.publication_failure.ok());
    EXPECT_EQ(actual.authority.value().control_generation, 1);
    auto completed = controller_->persistence_completed(
        actual.work_id, std::move(actual.authority), std::move(actual.publication_failure));
    ASSERT_TRUE(completed.ok());
    EXPECT_FALSE(completed.value().resolved->durable);
    start = request(OperationKind::Start, "published-but-uncertain").value();
    failed_step_ = persistence::FileStep::SyncDirectory;
    fail_ = true;
    actual = task::execute_control_work(*store_, *start.work);
    ASSERT_TRUE(actual.authority.ok());
    EXPECT_FALSE(actual.publication_failure.ok());
    EXPECT_EQ(actual.authority.value().control_generation, 2);
    completed = controller_->persistence_completed(
        actual.work_id, std::move(actual.authority), std::move(actual.publication_failure));
    ASSERT_TRUE(completed.ok());
    EXPECT_TRUE(completed.value().resolved->durable);
    EXPECT_TRUE(controller_->run_is_current(start.operation_id, 2));
}

TEST_F(
    TaskControllerTest,
    ConfigureRequiresQuiescenceAndMatchingActiveIdentityFactsBeforeAllocatingWork) {
    auto config = store_->load_config(initial_.active_config_version).value().config;
    rodaq::pipeline::StreamIdentityState identities;
    identities.task_id = id_;
    identities.observed_config_version = initial_.active_config_version;
    identities.pipeline_id_high_water = 10;
    identities.stream_id_high_water = 20;
    identities.streams.push_back({20, "sample", 1, 1, false, {}});
    auto changed = config;
    changed.name = "configured";
    task::ConfigureFacts facts{config, identities, false, true};
    EXPECT_EQ(
        controller_
            ->request_configure(changed, {"configure"}, {true, 1}, rodaq::WallTime{1s}, facts, {})
            .status()
            .code(),
        StatusCode::InvalidState);
    facts.quiescent = true;
    facts.deployment_allowed = false;
    EXPECT_EQ(
        controller_
            ->request_configure(changed, {"configure"}, {true, 1}, rodaq::WallTime{1s}, facts, {})
            .status()
            .code(),
        StatusCode::PermissionDenied);
    facts.deployment_allowed = true;
    identities.observed_config_version = "sha256:" + std::string(64, 'f');
    EXPECT_EQ(
        controller_
            ->request_configure(changed, {"configure"}, {true, 1}, rodaq::WallTime{1s}, facts, {})
            .status()
            .code(),
        StatusCode::DataLoss);
    EXPECT_EQ(controller_->durable_record()->control_generation, 1);
    EXPECT_TRUE(controller_->durable_record()->operation_history.entries.empty());
}

TEST_F(TaskControllerTest, ExhaustedIdentityAndIllegalTransitionsDoNotPublishNewIntent) {
    EXPECT_EQ(request(OperationKind::Resume, "resume").status().code(), StatusCode::InvalidState);
    EXPECT_EQ(request(OperationKind::Stop, "stop").status().code(), StatusCode::InvalidState);
    EXPECT_EQ(request(OperationKind::Reset, "reset").status().code(), StatusCode::InvalidArgument);
    auto exhausted = initial_;
    exhausted.control_generation = std::numeric_limits<std::uint64_t>::max();
    exhausted.record_revision = exhausted.control_generation;
    auto owner = task::TaskController::create(exhausted);
    ASSERT_TRUE(owner.ok());
    EXPECT_EQ(
        owner.value()
            ->request_lifecycle(
                OperationKind::Start, {"start"}, {}, rodaq::WallTime{1s}, {true, true})
            .status()
            .code(),
        StatusCode::ResourceExhausted);
    EXPECT_TRUE(owner.value()->durable_record()->operation_history.entries.empty());
}

TEST_F(TaskControllerTest, LifecycleAdmissionFactsFollowIdempotencyAndGenerationGuards) {
    EXPECT_EQ(
        controller_
            ->request_lifecycle(
                OperationKind::Start, {"stale"}, {true, 99}, rodaq::WallTime{1s}, {false, false})
            .status()
            .code(),
        StatusCode::Conflict);
    EXPECT_EQ(
        controller_
            ->request_lifecycle(
                OperationKind::Start, {"unsettled"}, {true, 1}, rodaq::WallTime{1s}, {false, true})
            .status()
            .code(),
        StatusCode::InvalidState);
    auto start = request(OperationKind::Start, "run", {true, 1}).value();
    auto pending = controller_->request_lifecycle(
        OperationKind::Start, {"run"}, {true, 1}, rodaq::WallTime{1s}, {false, false});
    ASSERT_TRUE(pending.ok());
    EXPECT_EQ(pending.value().operation_id, start.operation_id);
    ASSERT_TRUE(accept(start.work));
    auto accepted = controller_->request_lifecycle(
        OperationKind::Start, {"run"}, {true, 1}, rodaq::WallTime{1s}, {false, false});
    ASSERT_TRUE(accepted.ok());
    EXPECT_TRUE(accepted.value().durably_accepted);
    EXPECT_EQ(accepted.value().operation_id, start.operation_id);
    EXPECT_EQ(controller_->durable_record()->control_generation, 2);
}

}  // namespace
