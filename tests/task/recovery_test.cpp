#include "task/recovery_store.hpp"
#include "persistence/json.hpp"
#include "pipeline/admission_gate.hpp"

#include <gtest/gtest.h>

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
#include <string>
#include <utility>

namespace {
namespace task = rodaq::task;
namespace fs = std::filesystem;
namespace persistence = rodaq::persistence;
using rodaq::StatusCode;
using namespace std::chrono_literals;
constexpr auto kBoot = "11234567-89ab-4cde-8fab-0123456789ab";

class TaskRecoveryTest : public testing::Test {
protected:
    void SetUp() override {
        std::array<char, 40> pattern{};
        const std::string prefix = "/tmp/rodaq-recovery-XXXXXX";
        std::copy(prefix.begin(), prefix.end(), pattern.begin());
        const auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        record_.task_id = rodaq::parse_task_id("01234567-89ab-4cde-8fab-0123456789ab").value();
        record_.observed_control_generation = 7;
        record_.active_config_version = "sha256:" + std::string(64, 'a');
        task::RunRecovery run;
        run.operation_id.value[0] = 2;
        run.control_generation = 7;
        for (std::uint64_t id = 1; id <= 2; ++id) {
            task::AuthorizedPipeline entry;
            entry.pipeline_id = id;
            entry.instance_id.value[0] = static_cast<std::uint8_t>(id);
            entry.effective_config_id = "sha256:" + std::string(64, 'b');
            entry.prepared_boundary = {kBoot, 100 + id};
            entry.continuation_permission = true;
            run.authorized_pipelines.push_back(entry);
            record_.pipelines.push_back({7, entry});
        }
        record_.run_operation = run;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    auto open(persistence::FileFaultHook hook = {}, task::RecoveryPolicy policy = {}) {
        return task::TaskRecoveryStore::open(root_, record_.task_id, policy, std::move(hook));
    }

    task::TaskRecoveryRecord authorized() const {
        auto next = record_;
        ++next.record_revision;
        next.run_operation->phase = task::RunPhase::OpenAuthorized;
        next.run_operation->authorization_boundary = task::RecoveryBoundary{kBoot, 200};
        return next;
    }

    fs::path file() const { return root_ / (rodaq::to_string(record_.task_id) + ".json"); }

    fs::path root_;
    task::TaskRecoveryRecord record_;
};

TEST_F(TaskRecoveryTest, RoundTripPreservesFullAuthorizationAndExactUint64BeyondDoublePrecision) {
    auto next = authorized();
    next.record_revision = std::numeric_limits<std::uint64_t>::max();
    next.run_operation->authorization_boundary->monotonic_ns = 9007199254740993ULL;
    auto encoded = task::encode_recovery_record(next, {});
    ASSERT_TRUE(encoded.ok());
    EXPECT_NE(encoded.value().find("\"9007199254740993\""), std::string::npos);
    auto decoded = task::decode_recovery_record(encoded.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().record_revision, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(decoded.value().run_operation->authorized_pipelines.size(), 2);
    EXPECT_FALSE(decoded.value().run_operation->authorized_pipelines[0].opened_observed);
    EXPECT_EQ(decoded.value().run_operation->phase, task::RunPhase::OpenAuthorized);
    EXPECT_EQ(task::encode_recovery_record(decoded.value(), {}).value(), encoded.value());
}

TEST_F(TaskRecoveryTest, ClosedSchemaRejectsDuplicateFieldsTamperForeignDomainAndNumericIdentity) {
    auto encoded = task::encode_recovery_record(record_, {}).value();
    auto document = persistence::parse_json(encoded).value();
    document["record_revision"] = 1;
    EXPECT_FALSE(task::decode_recovery_record(document.dump(), {}).ok());
    document = persistence::parse_json(encoded).value();
    document["unexpected"] = nullptr;
    auto extra = persistence::encode_integrity(document, "rodaq.task-recovery.v1").value();
    EXPECT_FALSE(task::decode_recovery_record(extra, {}).ok());
    document = persistence::parse_json(encoded).value();
    document["observed_control_generation"] = "8";
    EXPECT_FALSE(task::decode_recovery_record(document.dump(), {}).ok());
    auto wrong_domain = persistence::encode_integrity(document, "rodaq.task-control.v1").value();
    EXPECT_FALSE(task::decode_recovery_record(wrong_domain, {}).ok());
    encoded.insert(1, "\"format_version\":\"1.0\",");
    EXPECT_FALSE(task::decode_recovery_record(encoded, {}).ok());
}

TEST_F(TaskRecoveryTest, IntegrityMatchesIndependentCanonicalAsciiGoldenVector) {
    auto encoded = task::encode_recovery_record(record_, {});
    ASSERT_TRUE(encoded.ok());
    // Independently calculated using Python sorted compact JSON, SHA256 and
    // the exact domain bytes including one terminal NUL.
    EXPECT_EQ(
        persistence::parse_json(encoded.value()).value().at("content_digest"),
        "sha256:f3cb997b7ce3fa266f2fdffbffefa7ca50c4843def95d18c0aa847981db1ccbc");
}

TEST_F(TaskRecoveryTest, InvalidPhaseEvidenceBoundsDuplicatesAndBootBoundariesAreRejected) {
    auto next = record_;
    next.run_operation->authorized_pipelines[0].opened_observed = true;
    EXPECT_FALSE(task::validate_recovery_record(next, {}).ok());
    next = authorized();
    next.run_operation->authorization_boundary.reset();
    EXPECT_FALSE(task::validate_recovery_record(next, {}).ok());
    next = authorized();
    next.run_operation->authorization_boundary->monotonic_ns = 1;
    EXPECT_FALSE(task::validate_recovery_record(next, {}).ok());
    next = authorized();
    next.run_operation->authorization_boundary->boot_uuid[0] = '2';
    EXPECT_FALSE(task::validate_recovery_record(next, {}).ok());
    next = record_;
    next.run_operation->authorized_pipelines[1] = next.run_operation->authorized_pipelines[0];
    EXPECT_FALSE(task::validate_recovery_record(next, {}).ok());
    task::RecoveryLimits limits;
    limits.max_pipelines = 1;
    EXPECT_FALSE(task::encode_recovery_record(record_, limits).ok());
    limits = {};
    limits.max_bytes = 1024;
    EXPECT_FALSE(task::encode_recovery_record(record_, limits).ok());
}

TEST_F(
    TaskRecoveryTest,
    StoreIsExclusiveAndPersistsPreparedThenAtomicFullSetAuthorizationAcrossRestart) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto store = std::move(opened.value());
    EXPECT_EQ(open().status().code(), StatusCode::Conflict);
    EXPECT_EQ(store->load().status().code(), StatusCode::NotFound);
    ASSERT_TRUE(store->publish(record_, 0).ok());
    auto next = authorized();
    ASSERT_TRUE(store->publish(next, 1).ok());
    EXPECT_EQ(store->publish(record_, 1).status().code(), StatusCode::Conflict);
    store.reset();
    opened = open();
    ASSERT_TRUE(opened.ok());
    auto loaded = opened.value()->load();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().run_operation->phase, task::RunPhase::OpenAuthorized);
    EXPECT_EQ(loaded.value().run_operation->authorized_pipelines.size(), 2);
    // No OpenedObserved is required to recover as possibly opened.
    EXPECT_FALSE(loaded.value().run_operation->authorized_pipelines[0].opened_observed);
    EXPECT_FALSE(fs::exists(root_ / "control"));
}

TEST_F(
    TaskRecoveryTest,
    FailedAuthorizationNeverOpensGateAndUncertainPublishRequiresActualReconciliation) {
    std::atomic<bool> fail{false};
    auto opened = open(
        [&](auto step) { return fail && step == persistence::FileStep::SyncDirectory ? EIO : 0; });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.publish(record_, 0).ok());
    rodaq::pipeline::AdmissionGate gate(
        record_.run_operation->authorized_pipelines[0].instance_id, 7);
    fail = true;
    const auto result = store.publish(authorized(), 1);
    if (result.ok()) {
        (void)gate.open_admission(record_.run_operation->authorized_pipelines[0].instance_id, 7);
    }
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(gate.snapshot().open);
    EXPECT_EQ(store.load().status().code(), StatusCode::Unavailable);
    EXPECT_FALSE(store.publish(authorized(), 1).ok());
    EXPECT_FALSE(store.reconcile().ok());
    fail = false;
    auto reconciled = store.reconcile();
    ASSERT_TRUE(reconciled.ok());
    ASSERT_TRUE(reconciled.value());
    EXPECT_EQ(reconciled.value()->run_operation->phase, task::RunPhase::OpenAuthorized);
    EXPECT_EQ(reconciled.value()->record_revision, 2);
    EXPECT_FALSE(gate.snapshot().open);
}

TEST_F(TaskRecoveryTest, FailedBeforeRenameReconcilesPreviousAuthorityAndRetriesSameRevision) {
    std::atomic<bool> fail{false};
    auto opened =
        open([&](auto step) { return fail && step == persistence::FileStep::Publish ? EIO : 0; });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.publish(record_, 0).ok());
    fail = true;
    EXPECT_FALSE(store.publish(authorized(), 1).ok());
    fail = false;
    auto reconciled = store.reconcile();
    ASSERT_TRUE(reconciled.ok());
    ASSERT_TRUE(reconciled.value());
    EXPECT_EQ(reconciled.value()->record_revision, 1);
    EXPECT_EQ(reconciled.value()->run_operation->phase, task::RunPhase::Prepared);
    EXPECT_TRUE(store.publish(authorized(), 1).ok());
}

TEST_F(TaskRecoveryTest, ActualPersistenceSingleFlightRejectsConcurrentWorkUntilRealReturn) {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    bool delay = false;
    auto opened = open([&](auto step) {
        if (delay && step == persistence::FileStep::SyncFile) {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return release; });
        }
        return 0;
    });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.publish(record_, 0).ok());
    delay = true;
    auto work = std::async(std::launch::async, [&] { return store.publish(authorized(), 1); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return entered; }));
    }
    EXPECT_EQ(work.wait_for(1ms), std::future_status::timeout);
    EXPECT_EQ(store.load().status().code(), StatusCode::Conflict);
    EXPECT_EQ(store.publish(authorized(), 1).status().code(), StatusCode::Conflict);
    EXPECT_EQ(store.reconcile().status().code(), StatusCode::Conflict);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    condition.notify_all();
    EXPECT_TRUE(work.get().ok());
    EXPECT_EQ(store.load().value().record_revision, 2);
}

TEST_F(TaskRecoveryTest, WriteAmplificationBudgetRejectsCheckpointWithoutChangingDurableAuthority) {
    task::RecoveryPolicy policy;
    policy.max_publications_per_window = 1;
    policy.publication_window = 1h;
    auto opened = open({}, policy);
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.publish(record_, 0).ok());
    EXPECT_EQ(store.publish(authorized(), 1).status().code(), StatusCode::ResourceExhausted);
    EXPECT_EQ(store.load().value().record_revision, 1);
}

TEST_F(TaskRecoveryTest, MissingOrUnexpectedAuthorityProtectsAndCannotBeRecreated) {
    auto opened = open();
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.publish(record_, 0).ok());
    ASSERT_TRUE(fs::remove(file()));
    EXPECT_EQ(store.load().status().code(), StatusCode::DataLoss);
    EXPECT_FALSE(store.reconcile().ok());
    EXPECT_FALSE(store.publish(record_, 0).ok());
    auto stranger = authorized();
    stranger.record_revision = 9;
    std::ofstream(file()) << task::encode_recovery_record(stranger, {}).value();
    EXPECT_EQ(store.reconcile().status().code(), StatusCode::DataLoss);
}

TEST_F(
    TaskRecoveryTest,
    VersionOrderFreezesAuthorizationAndRetainsUnendedInstanceEvidenceOnSupersession) {
    auto old = authorized();
    auto next = old;
    ++next.record_revision;
    next.run_operation->authorized_pipelines[0].effective_config_id =
        "sha256:" + std::string(64, 'c');
    EXPECT_FALSE(task::recovery_successor(old, next).ok());
    next = old;
    ++next.record_revision;
    next.run_operation->phase = task::RunPhase::Prepared;
    EXPECT_FALSE(task::recovery_successor(old, next).ok());
    next = old;
    ++next.record_revision;
    ++next.observed_control_generation;
    next.run_operation.reset();
    next.pipelines.clear();
    EXPECT_FALSE(task::recovery_successor(old, next).ok());
    for (const auto& entry : old.run_operation->authorized_pipelines) {
        next.cleanup_intents.push_back({entry.pipeline_id, entry.instance_id, 7});
    }
    EXPECT_TRUE(task::recovery_successor(old, next).ok());
    auto ended = old;
    ++ended.record_revision;
    ended.run_operation->phase = task::RunPhase::Ended;
    for (auto& entry : ended.run_operation->authorized_pipelines) {
        entry.ended_boundary = task::RecoveryBoundary{kBoot, 300};
    }
    EXPECT_TRUE(task::validate_recovery_record(ended, {}).ok());
    EXPECT_TRUE(task::recovery_successor(old, ended).ok());
}

TEST_F(TaskRecoveryTest, ResetAndCleanupAreBoundedTypedFactsWithoutControlIntent) {
    task::ResetProgress reset;
    reset.operation_id.value[0] = 3;
    reset.reset_generation = 1;
    task::VolumeResetProgress volume;
    volume.volume_id.value[0] = 4;
    reset.volumes.push_back(volume);
    record_.reset_progress = reset;
    record_.cleanup_intents.push_back(
        {1, record_.run_operation->authorized_pipelines[0].instance_id, 7});
    auto encoded = task::encode_recovery_record(record_, {});
    ASSERT_TRUE(encoded.ok());
    auto decoded = task::decode_recovery_record(encoded.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().reset_progress->reset_generation, 1);
    EXPECT_EQ(decoded.value().cleanup_intents.size(), 1);
    record_.reset_progress->volumes[0].ledger_settled = true;
    EXPECT_FALSE(task::encode_recovery_record(record_, {}).ok());
    record_.reset_progress->volumes[0].files_removed = true;
    record_.reset_progress->volumes[0].catalog_settled = true;
    EXPECT_TRUE(task::encode_recovery_record(record_, {}).ok());
    auto next = record_;
    ++next.record_revision;
    next.reset_progress->volumes[0] = volume;
    EXPECT_FALSE(task::recovery_successor(record_, next).ok());
    next = record_;
    ++next.record_revision;
    next.reset_progress->operation_id.value[0] = 5;
    EXPECT_FALSE(task::recovery_successor(record_, next).ok());
}

class RecoveryFaultTest : public TaskRecoveryTest,
                          public testing::WithParamInterface<persistence::FileStep> {};

TEST_P(RecoveryFaultTest, EveryPublicationFailureReconcilesOnlyActualOldOrNewAuthority) {
    std::atomic<bool> fail{false};
    auto opened = open([&](auto step) { return fail && step == GetParam() ? EIO : 0; });
    ASSERT_TRUE(opened.ok());
    auto& store = *opened.value();
    ASSERT_TRUE(store.publish(record_, 0).ok());
    fail = true;
    EXPECT_FALSE(store.publish(authorized(), 1).ok());
    EXPECT_FALSE(store.load().ok());
    fail = false;
    auto reconciled = store.reconcile();
    ASSERT_TRUE(reconciled.ok());
    ASSERT_TRUE(reconciled.value());
    const auto expected = GetParam() == persistence::FileStep::SyncDirectory ? 2U : 1U;
    EXPECT_EQ(reconciled.value()->record_revision, expected);
    EXPECT_EQ(
        reconciled.value()->run_operation->phase,
        expected == 2 ? task::RunPhase::OpenAuthorized : task::RunPhase::Prepared);
}

INSTANTIATE_TEST_SUITE_P(
    RecoveryFileFailures, RecoveryFaultTest,
    testing::Values(
        persistence::FileStep::OpenDirectory, persistence::FileStep::CreateTemporary,
        persistence::FileStep::WriteTemporary, persistence::FileStep::SyncFile,
        persistence::FileStep::Publish, persistence::FileStep::SyncDirectory));

}  // namespace
