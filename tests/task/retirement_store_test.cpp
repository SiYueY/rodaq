#include "task/retirement_store.hpp"

#include "persistence/atomic_file.hpp"
#include "persistence/json.hpp"
#include "task/identity.hpp"
#include "time/wall_time.hpp"

#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;
using rodaq::OperationId;
using rodaq::OperationKind;
using rodaq::OperationState;
using rodaq::RequestIdentity;
using rodaq::StatusCode;
using rodaq::TaskId;
using rodaq::WallTime;
using rodaq::persistence::FileStep;
using rodaq::task::RetirementCollectProof;
using rodaq::task::TaskRetirementRecord;
using rodaq::task::TaskRetirementStore;

struct TempDir {
    fs::path path;

    TempDir() {
        auto pattern = (fs::temp_directory_path() / "rodaq-retire-store-XXXXXX").string();
        char* created = mkdtemp(pattern.data());
        EXPECT_NE(created, nullptr);
        path = created ? created : "";
    }

    ~TempDir() {
        std::error_code error;
        if (!path.empty()) {
            fs::remove_all(path, error);
        }
    }
};

TaskRetirementRecord sample_record(
    std::string_view task_uuid = "11111111-1111-4111-8111-111111111111",
    std::string_view op_uuid = "22222222-2222-4222-8222-222222222222",
    std::string_view key = "retire-req-1") {
    TaskRetirementRecord record;
    record.task_id = rodaq::parse_task_id(task_uuid).value();
    record.retire_operation_id = rodaq::parse_operation_id(op_uuid).value();
    record.request = RequestIdentity{std::string(key)};
    record.request_digest =
        rodaq::task::lifecycle_request_digest(record.task_id, OperationKind::Retire, {}).value();
    record.last_control_generation = 7;
    record.last_config_version =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    record.retired_at = rodaq::time::parse_wall_time("2026-11-26T00:00:00.000000000Z").value();
    return record;
}

TEST(RetirementStoreTest, GoldenDigestAndRoundTrip) {
    const auto record = sample_record();
    auto encoded = rodaq::task::encode_retirement_record(record);
    ASSERT_TRUE(encoded.ok());

    auto parsed = rodaq::persistence::parse_json(encoded.value());
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().at("format_version"), "1.0");
    ASSERT_EQ(parsed.value().at("terminal_result"), "succeeded");
    ASSERT_EQ(parsed.value().at("last_control_generation"), "7");

    // Recompute expected digest independently with domain prefix rodaq.retired-task.v1\0.
    auto without_digest = parsed.value();
    without_digest.erase("content_digest");
    auto expected_digest =
        rodaq::persistence::semantic_digest(without_digest, "rodaq.retired-task.v1");
    ASSERT_TRUE(expected_digest.ok());
    EXPECT_EQ(parsed.value().at("content_digest"), expected_digest.value());

    auto decoded = rodaq::task::decode_retirement_record(encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().task_id, record.task_id);
    EXPECT_EQ(decoded.value().retire_operation_id, record.retire_operation_id);
    EXPECT_EQ(decoded.value().request.request_key, record.request.request_key);
    EXPECT_EQ(decoded.value().request_digest, record.request_digest);
    EXPECT_EQ(decoded.value().last_control_generation, 7u);
    EXPECT_EQ(decoded.value().last_config_version, record.last_config_version);
    EXPECT_EQ(decoded.value().retired_at, record.retired_at);

    const auto op = rodaq::task::retirement_operation_snapshot(decoded.value());
    EXPECT_EQ(op.operation_id, record.retire_operation_id);
    EXPECT_EQ(op.kind, OperationKind::Retire);
    EXPECT_EQ(op.state, OperationState::Succeeded);
    EXPECT_EQ(op.control_generation, 7u);
}

TEST(RetirementStoreTest, RejectsCorruptionAndInvalidTerminalResult) {
    const auto record = sample_record();
    auto encoded = rodaq::task::encode_retirement_record(record);
    ASSERT_TRUE(encoded.ok());

    // Tamper with control generation.
    auto tampered = encoded.value();
    const auto pos = tampered.find("\"7\"");
    ASSERT_NE(pos, std::string::npos);
    tampered.replace(pos, 3, "\"8\"");
    EXPECT_EQ(
        rodaq::task::decode_retirement_record(tampered).status().code(), StatusCode::DataLoss);

    // Non-succeeded terminal_result is invalid even with recomputed digest.
    auto parsed = rodaq::persistence::parse_json(encoded.value()).value();
    parsed["terminal_result"] = "failed";
    parsed.erase("content_digest");
    auto reencoded = rodaq::persistence::encode_integrity(parsed, "rodaq.retired-task.v1");
    ASSERT_TRUE(reencoded.ok());
    EXPECT_EQ(
        rodaq::task::decode_retirement_record(reencoded.value()).status().code(),
        StatusCode::DataLoss);
}

TEST(RetirementStoreTest, PublishReloadIdempotencyAndConflict) {
    TempDir dir;
    const auto record = sample_record();
    {
        auto store = TaskRetirementStore::open(dir.path, 4);
        ASSERT_TRUE(store.ok());
        EXPECT_FALSE(store.value()->contains(record.task_id).value());
        ASSERT_TRUE(store.value()->publish(record).ok());
        EXPECT_TRUE(store.value()->contains(record.task_id).value());

        // Idempotent publish of exact same tombstone succeeds.
        EXPECT_TRUE(store.value()->publish(record).ok());

        // Conflicting tombstone for same task_id is rejected.
        auto conflicting = record;
        conflicting.last_control_generation = 9;
        EXPECT_EQ(store.value()->publish(conflicting).status().code(), StatusCode::Conflict);

        // Request lookup: exact match returns operation_id; different digest returns Conflict.
        auto matched =
            store.value()->lookup_request(record.task_id, record.request, record.request_digest);
        ASSERT_TRUE(matched.ok());
        EXPECT_EQ(matched.value(), record.retire_operation_id);

        const std::string other_digest =
            "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
        EXPECT_EQ(
            store.value()
                ->lookup_request(record.task_id, record.request, other_digest)
                .status()
                .code(),
            StatusCode::Conflict);
        EXPECT_EQ(
            store.value()
                ->lookup_request(
                    record.task_id, RequestIdentity{"other-key"}, record.request_digest)
                .status()
                .code(),
            StatusCode::NotFound);
    }

    // Reopen recovers tombstone durably.
    auto reopened = TaskRetirementStore::open(dir.path, 4);
    ASSERT_TRUE(reopened.ok());
    EXPECT_TRUE(reopened.value()->contains(record.task_id).value());
    auto loaded = reopened.value()->load(record.task_id);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().retire_operation_id, record.retire_operation_id);
}

TEST(RetirementStoreTest, FailClosedGcRequiresAllProofConditions) {
    TempDir dir;
    auto store = TaskRetirementStore::open(dir.path, 4);
    ASSERT_TRUE(store.ok());
    const auto record = sample_record();
    ASSERT_TRUE(store.value()->publish(record).ok());

    const auto audit = std::chrono::hours{24};
    const auto expired_now = record.retired_at + audit;

    // Not expired yet -> rejected.
    RetirementCollectProof not_expired{
        record.retired_at + std::chrono::hours{23}, audit, true, true, true};
    EXPECT_EQ(
        store.value()->collect(record.task_id, not_expired).status().code(),
        StatusCode::InvalidState);

    // Clock moved backwards -> rejected.
    RetirementCollectProof backward{
        record.retired_at - std::chrono::seconds{1}, audit, true, true, true};
    EXPECT_EQ(
        store.value()->collect(record.task_id, backward).status().code(), StatusCode::InvalidState);

    // Active namespace not removed -> rejected.
    RetirementCollectProof active_left{expired_now, audit, false, true, true};
    EXPECT_EQ(
        store.value()->collect(record.task_id, active_left).status().code(),
        StatusCode::InvalidState);

    // Artifact references still exist -> rejected.
    RetirementCollectProof has_refs{expired_now, audit, true, false, true};
    EXPECT_EQ(
        store.value()->collect(record.task_id, has_refs).status().code(), StatusCode::InvalidState);

    // Maintenance still pending -> rejected.
    RetirementCollectProof pending_maint{expired_now, audit, true, true, false};
    EXPECT_EQ(
        store.value()->collect(record.task_id, pending_maint).status().code(),
        StatusCode::InvalidState);

    // All proofs satisfied -> succeeds and removes tombstone durably.
    RetirementCollectProof valid{expired_now, audit, true, true, true};
    ASSERT_TRUE(store.value()->collect(record.task_id, valid).ok());
    EXPECT_FALSE(store.value()->contains(record.task_id).value());
    EXPECT_FALSE(fs::exists(dir.path / (rodaq::to_string(record.task_id) + ".json")));
}

TEST(RetirementStoreTest, UncertainPublishAndGcFaultsFailClosedUntilReconciled) {
    TempDir dir;
    bool fail_sync_dir = false;
    auto store = TaskRetirementStore::open(dir.path, 4, [&](FileStep step) {
        if (fail_sync_dir && step == FileStep::SyncDirectory) {
            return EIO;
        }
        return 0;
    });
    ASSERT_TRUE(store.ok());
    const auto record = sample_record();

    fail_sync_dir = true;
    auto published = store.value()->publish(record);
    ASSERT_FALSE(published.ok());
    // Store is protected until reconciled.
    EXPECT_EQ(store.value()->contains(record.task_id).status().code(), StatusCode::Unavailable);

    fail_sync_dir = false;
    ASSERT_TRUE(store.value()->reconcile().ok());
    // File was already renamed before SyncDirectory failed -> reconcile recovers it durably!
    EXPECT_TRUE(store.value()->contains(record.task_id).value());

    // Fail SyncDirectory during GC -> protected until reconciled, then reflects actual absence.
    fail_sync_dir = true;
    const RetirementCollectProof proof{
        record.retired_at + std::chrono::hours{48}, std::chrono::hours{24}, true, true, true};
    EXPECT_FALSE(store.value()->collect(record.task_id, proof).ok());
    EXPECT_EQ(store.value()->contains(record.task_id).status().code(), StatusCode::Unavailable);

    fail_sync_dir = false;
    ASSERT_TRUE(store.value()->reconcile().ok());
    EXPECT_FALSE(store.value()->contains(record.task_id).value());
}

TEST(RetirementStoreTest, CapacityEnforcedAndCorruptedFileBlocksOpen) {
    TempDir dir;
    {
        auto store = TaskRetirementStore::open(dir.path, 1);
        ASSERT_TRUE(store.ok());
        ASSERT_TRUE(store.value()->publish(sample_record()).ok());
        const auto second = sample_record(
            "33333333-3333-4333-8333-333333333333", "44444444-4444-4444-8444-444444444444",
            "retire-req-2");
        EXPECT_EQ(store.value()->publish(second).status().code(), StatusCode::ResourceExhausted);
    }

    // Corrupt tombstone on disk -> open fails closed.
    std::ofstream out(dir.path / "11111111-1111-4111-8111-111111111111.json", std::ios::trunc);
    out << "{corrupted";
    out.close();
    auto reopened = TaskRetirementStore::open(dir.path, 2);
    EXPECT_EQ(reopened.status().code(), StatusCode::DataLoss);
}

}  // namespace
