#include "task/task_store.hpp"
#include "task/identity.hpp"
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
#include <mutex>
#include <string>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace task = rodaq::task;
namespace persistence = rodaq::persistence;
using rodaq::DesiredState;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::StatusCode;
using namespace std::chrono_literals;

class TaskStoreTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-task-store-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        task_directory_ = root_ / "task";
        controls_ = root_ / "control";
        ASSERT_TRUE(std::filesystem::create_directories(task_directory_ / "configs"));
        ASSERT_TRUE(std::filesystem::create_directory(controls_));
        task_id_ = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        std::ifstream input(RODAQ_TASK_CONFIG_FIXTURE);
        auto parsed = rodaq::parse_task_config(persistence::Json::parse(input).dump());
        ASSERT_TRUE(parsed.ok());
        config_ = std::move(parsed).value();
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    auto open(
        task::TaskStoreMode mode = task::TaskStoreMode::Materializing,
        persistence::FileFaultHook faults = {}, task::TaskStorePolicy policy = {}) {
        return task::TaskStore::open(
            task_directory_, controls_, task_id_, mode, policy, std::move(faults));
    }

    task::TaskControlRecord initialize(task::TaskStore& store) {
        auto created = store.create_config(config_);
        EXPECT_TRUE(created.ok());
        task::TaskControlRecord record;
        record.task_id = task_id_;
        if (created.ok()) {
            record.active_config_version = created.value().version;
            EXPECT_TRUE(store.publish_control(record, 0).ok());
        }
        return record;
    }

    task::TaskControlRecord intent(
        const task::TaskControlRecord& previous, OperationKind kind = OperationKind::Start) {
        auto next = previous;
        ++next.control_generation;
        ++next.record_revision;
        if (next.current_operation_id) {
            auto& old = next.operation_history.entries.back().snapshot;
            if (!task::terminal(old.state)) {
                old.state = OperationState::Superseded;
                old.terminal_at = rodaq::WallTime{2s};
            }
        }
        task::StoredOperation operation;
        operation.request.request_key = "intent-" + std::to_string(next.control_generation);
        operation.request_digest = task::lifecycle_request_digest(task_id_, kind, {}).value();
        operation.snapshot.operation_id.value[15] =
            static_cast<std::uint8_t>(next.control_generation);
        operation.snapshot.kind = kind;
        operation.snapshot.control_generation = next.control_generation;
        operation.snapshot.accepted_at = rodaq::WallTime{1s};
        next.current_operation_id = operation.snapshot.operation_id;
        next.operation_history.entries.push_back(std::move(operation));
        next.desired_state =
            kind == OperationKind::Stop ? DesiredState::Stopped : DesiredState::Running;
        return next;
    }

    void replace_control(const task::TaskControlRecord& record) {
        auto bytes = task::encode_control_record(record, {});
        ASSERT_TRUE(bytes.ok());
        auto published = persistence::publish_file(
            controls_, rodaq::to_string(task_id_) + ".json", bytes.value(),
            persistence::FilePublishMode::Replace, 1024 * 1024);
        ASSERT_TRUE(published.status.ok());
    }

    std::filesystem::path root_;
    std::filesystem::path task_directory_;
    std::filesystem::path controls_;
    rodaq::TaskId task_id_;
    rodaq::TaskConfig config_;
};

TEST_F(
    TaskStoreTest, ImmutableConfigAndInitialControlReopenWithExactIdentityAndExclusiveOwnership) {
    EXPECT_EQ(open(task::TaskStoreMode::Existing).status().code(), StatusCode::DataLoss);
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened).value();
    EXPECT_EQ(open().status().code(), StatusCode::Conflict);
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "other-task" / "configs"));
    EXPECT_EQ(
        task::TaskStore::open(
            root_ / "other-task", controls_, task_id_, task::TaskStoreMode::Materializing)
            .status()
            .code(),
        StatusCode::Conflict);
    auto initial = initialize(*store);
    auto duplicate = store->create_config(config_);
    ASSERT_TRUE(duplicate.ok());
    EXPECT_EQ(duplicate.value().version, initial.active_config_version);
    auto loaded = store->load_control();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().record_revision, 1);
    EXPECT_EQ(store->publish_control(initial, 0).status().code(), StatusCode::Conflict);
    store.reset();
    auto reopened = open(task::TaskStoreMode::Existing);
    ASSERT_TRUE(reopened.ok());
    auto config = reopened.value()->load_config(initial.active_config_version);
    ASSERT_TRUE(config.ok());
    EXPECT_EQ(config.value().config.pipelines[0].streams[0].stream_id, 20);
    EXPECT_EQ(reopened.value()->load_control().value().desired_state, DesiredState::Stopped);
}

TEST_F(TaskStoreTest, GenerationAndStatusRevisionOrderRejectStaleIntentBeforeNativePublish) {
    std::atomic<unsigned> publications{0};
    auto opened = open(task::TaskStoreMode::Materializing, [&](persistence::FileStep step) {
        if (step == persistence::FileStep::Publish) {
            ++publications;
        }
        return 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    auto started = intent(initial);
    ASSERT_TRUE(store.publish_control(started, 1).ok());
    auto status = started;
    ++status.record_revision;
    status.operation_history.entries[0].snapshot.state = OperationState::InProgress;
    ASSERT_TRUE(store.publish_control(status, 2).ok());
    EXPECT_EQ(status.control_generation, 2);
    const auto count = publications.load();
    auto obsolete_stop = intent(started, OperationKind::Stop);
    EXPECT_EQ(store.publish_control(obsolete_stop, 2).status().code(), StatusCode::Conflict);
    EXPECT_EQ(publications.load(), count);
    auto stop = intent(status, OperationKind::Stop);
    ASSERT_TRUE(store.publish_control(stop, 3).ok());
    EXPECT_EQ(store.load_control().value().control_generation, 3);
    EXPECT_EQ(store.publish_control(started, 1).status().code(), StatusCode::Conflict);
}

TEST_F(TaskStoreTest, UnknownFsyncOutcomeMustReconcileActualCandidateBeforeSupersedingStop) {
    std::atomic<bool> fail{false};
    auto opened = open(task::TaskStoreMode::Materializing, [&](persistence::FileStep step) {
        return fail && step == persistence::FileStep::SyncDirectory ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    const auto started = intent(initial);
    fail = true;
    EXPECT_FALSE(store.publish_control(started, 1).ok());
    EXPECT_EQ(store.load_control().status().code(), StatusCode::Unavailable);
    EXPECT_EQ(
        store.publish_control(intent(started, OperationKind::Stop), 2).status().code(),
        StatusCode::Unavailable);
    EXPECT_FALSE(store.reconcile().ok());
    fail = false;
    auto actual = store.reconcile();
    ASSERT_TRUE(actual.ok());
    ASSERT_TRUE(actual.value());
    EXPECT_EQ(actual.value()->control_generation, 2);
    const auto stop = intent(*actual.value(), OperationKind::Stop);
    ASSERT_TRUE(store.publish_control(stop, actual.value()->record_revision).ok());
    EXPECT_EQ(store.publish_control(started, 1).status().code(), StatusCode::Conflict);
}

TEST_F(TaskStoreTest, FailureBeforeRenameReconcilesOldAuthorityAndDoesNotPretendIntentAccepted) {
    std::atomic<bool> fail{false};
    auto opened = open(task::TaskStoreMode::Materializing, [&](persistence::FileStep step) {
        return fail && step == persistence::FileStep::SyncFile ? EIO : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    fail = true;
    EXPECT_FALSE(store.publish_control(intent(initial), 1).ok());
    fail = false;
    auto actual = store.reconcile();
    ASSERT_TRUE(actual.ok());
    ASSERT_TRUE(actual.value());
    EXPECT_EQ(actual.value()->control_generation, 1);
    EXPECT_TRUE(store.publish_control(intent(*actual.value()), 1).ok());
}

TEST_F(TaskStoreTest, SingleFlightCoversSlowParentFsyncAndKeepsConcurrentWorkOutOfFilesystem) {
    std::mutex mutex;
    std::condition_variable condition;
    bool block = false;
    bool entered = false;
    bool release = false;
    auto opened = open(task::TaskStoreMode::Materializing, [&](persistence::FileStep step) {
        std::unique_lock<std::mutex> lock(mutex);
        if (block && step == persistence::FileStep::SyncDirectory) {
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        return 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    const auto start = intent(initial);
    block = true;
    auto first = std::async(std::launch::async, [&] { return store.publish_control(start, 1); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 5s, [&] { return entered; }));
    }
    auto second = std::async(std::launch::async, [&] {
        return store.publish_control(intent(start, OperationKind::Stop), 2);
    });
    const auto ready = second.wait_for(1s);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        condition.notify_all();
    }
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_EQ(second.get().status().code(), StatusCode::Conflict);
    EXPECT_TRUE(first.get().ok());
    EXPECT_EQ(store.load_control().value().control_generation, 2);
}

TEST_F(TaskStoreTest, OutsideMutationCannotRollbackProvenAuthorityOrBeBlindlyAdopted) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    const auto started = intent(initial);
    ASSERT_TRUE(store.publish_control(started, 1).ok());
    replace_control(initial);
    EXPECT_EQ(store.load_control().status().code(), StatusCode::DataLoss);
    EXPECT_EQ(store.reconcile().status().code(), StatusCode::DataLoss);
    EXPECT_EQ(
        store.publish_control(intent(started, OperationKind::Stop), 2).status().code(),
        StatusCode::Unavailable);
}

TEST_F(TaskStoreTest, DurableConfigBudgetAndReferenceChecksPreserveExistingSnapshots) {
    task::TaskStorePolicy policy;
    policy.max_config_versions = 2;
    auto opened = open(task::TaskStoreMode::Materializing, {}, policy);
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    auto next_config = config_;
    next_config.name = "another version";
    auto second = store.create_config(next_config);
    ASSERT_TRUE(second.ok());
    next_config.name = "third";
    EXPECT_EQ(store.create_config(next_config).status().code(), StatusCode::ResourceExhausted);
    EXPECT_TRUE(store.load_config(initial.active_config_version).ok());
    auto illegal_status = initial;
    ++illegal_status.record_revision;
    illegal_status.active_config_version = second.value().version;
    EXPECT_EQ(store.publish_control(illegal_status, 1).status().code(), StatusCode::Conflict);
    auto missing = intent(initial);
    missing.desired_state = DesiredState::Stopped;
    missing.operation_history.entries.back().snapshot.kind = OperationKind::Configure;
    missing.operation_history.entries.back().request_digest =
        task::configure_request_digest(task_id_, config_, {}, {}).value();
    missing.active_config_version = "sha256:" + std::string(64, '0');
    EXPECT_EQ(store.publish_control(missing, 1).status().code(), StatusCode::NotFound);
    EXPECT_EQ(store.load_config("../escape").status().code(), StatusCode::InvalidArgument);
    next_config.pipelines[0].streams[0].stream_id = 0;
    EXPECT_FALSE(store.create_config(next_config).ok());
}

TEST_F(TaskStoreTest, TerminalHistoryCannotRevertOrChangeAcceptedIdentity) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    auto start = intent(initial);
    ASSERT_TRUE(store.publish_control(start, 1).ok());
    ++start.record_revision;
    start.operation_history.entries[0].snapshot.state = OperationState::Succeeded;
    start.operation_history.entries[0].snapshot.terminal_at = rodaq::WallTime{2s};
    ASSERT_TRUE(store.publish_control(start, 2).ok());
    auto reverted = start;
    ++reverted.record_revision;
    reverted.operation_history.entries[0].snapshot.state = OperationState::Failed;
    EXPECT_EQ(store.publish_control(reverted, 3).status().code(), StatusCode::Conflict);
    reverted = start;
    ++reverted.record_revision;
    reverted.operation_history.entries[0].request.request_key = "other-key";
    EXPECT_EQ(store.publish_control(reverted, 3).status().code(), StatusCode::Conflict);
}

TEST_F(TaskStoreTest, StartupCleansOwnedConfigTempsButNeverSilentlySkipsForeignFiles) {
    const auto temporary = task_directory_ / "configs" / (".rodaq-tmp-" + std::string(32, 'a'));
    std::ofstream(temporary) << "unpublished";
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    EXPECT_FALSE(std::filesystem::exists(temporary));
    const auto initial = initialize(*opened.value());
    opened.value().reset();
    std::ofstream(task_directory_ / "configs" / "foreign") << "unknown";
    EXPECT_EQ(open(task::TaskStoreMode::Existing).status().code(), StatusCode::DataLoss);
    std::filesystem::remove(task_directory_ / "configs" / "foreign");
    const auto config_path =
        task_directory_ / "configs" / (initial.active_config_version.substr(7) + ".json");
    std::ofstream(config_path) << "{}";
    EXPECT_EQ(open(task::TaskStoreMode::Existing).status().code(), StatusCode::DataLoss);
}

TEST_F(TaskStoreTest, ProcessExitAfterControlRenameRecoversInitialIntentAndTerminalPublications) {
    for (unsigned phase = 0; phase < 3; ++phase) {
        const auto directory = root_ / std::to_string(phase);
        const auto task_directory = directory / "task";
        const auto controls = directory / "control";
        ASSERT_TRUE(std::filesystem::create_directories(task_directory / "configs"));
        ASSERT_TRUE(std::filesystem::create_directory(controls));
        const auto child = fork();
        ASSERT_GE(child, 0);
        if (child == 0) {
            bool armed = false;
            auto opened = task::TaskStore::open(
                task_directory, controls, task_id_, task::TaskStoreMode::Materializing, {},
                [&](persistence::FileStep step) {
                    if (armed && step == persistence::FileStep::SyncDirectory) {
                        _exit(91);
                    }
                    return 0;
                });
            if (!opened.ok()) {
                _exit(90);
            }
            auto config = opened.value()->create_config(config_);
            if (!config.ok()) {
                _exit(90);
            }
            task::TaskControlRecord initial;
            initial.task_id = task_id_;
            initial.active_config_version = config.value().version;
            armed = phase == 0;
            if (!opened.value()->publish_control(initial, 0).ok()) {
                _exit(90);
            }
            auto start = intent(initial);
            armed = phase == 1;
            if (!opened.value()->publish_control(start, 1).ok()) {
                _exit(90);
            }
            ++start.record_revision;
            start.operation_history.entries[0].snapshot.state = OperationState::Succeeded;
            start.operation_history.entries[0].snapshot.terminal_at = rodaq::WallTime{2s};
            armed = true;
            (void)opened.value()->publish_control(start, 2);
            _exit(90);
        }
        int status = 0;
        ASSERT_EQ(waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 91);
        auto recovered = task::TaskStore::open(
            task_directory, controls, task_id_, task::TaskStoreMode::Existing);
        ASSERT_TRUE(recovered.ok()) << static_cast<int>(recovered.status().code());
        auto actual = recovered.value()->load_control();
        ASSERT_TRUE(actual.ok());
        EXPECT_EQ(actual.value().record_revision, phase + 1);
        EXPECT_EQ(actual.value().control_generation, phase == 0 ? 1u : 2u);
        if (phase != 0) {
            EXPECT_EQ(
                actual.value().operation_history.entries[0].snapshot.state,
                phase == 1 ? OperationState::Accepted : OperationState::Succeeded);
        }
    }
}

TEST_F(TaskStoreTest, FailedTemporaryCleanupRetainsProtectionUntilOwnerReconciliation) {
    std::atomic<bool> fail{false};
    auto opened = open(task::TaskStoreMode::Materializing, [&](persistence::FileStep step) {
        return fail && (step == persistence::FileStep::WriteTemporary ||
                        step == persistence::FileStep::RemoveTemporary)
                   ? EIO
                   : 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    const auto initial = initialize(store);
    fail = true;
    EXPECT_FALSE(store.publish_control(intent(initial), 1).ok());
    EXPECT_EQ(store.publish_control(intent(initial), 1).status().code(), StatusCode::Unavailable);
    unsigned temporaries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(controls_)) {
        temporaries +=
            persistence::is_temporary_file_name(entry.path().filename().string()) ? 1u : 0u;
    }
    EXPECT_EQ(temporaries, 1);
    fail = false;
    auto reconciled = store.reconcile();
    ASSERT_TRUE(reconciled.ok());
    ASSERT_TRUE(reconciled.value());
    EXPECT_EQ(reconciled.value()->control_generation, 1);
    for (const auto& entry : std::filesystem::directory_iterator(controls_)) {
        EXPECT_FALSE(persistence::is_temporary_file_name(entry.path().filename().string()));
    }
    EXPECT_TRUE(store.publish_control(intent(initial), 1).ok());
}

}  // namespace
